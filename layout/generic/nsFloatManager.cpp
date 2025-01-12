/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* class that manages rules for positioning floats */

#include "nsFloatManager.h"

#include <algorithm>
#include <initializer_list>

#include "gfxContext.h"
#include "mozilla/ReflowInput.h"
#include "mozilla/ShapeUtils.h"
#include "nsBlockFrame.h"
#include "nsError.h"
#include "nsImageRenderer.h"
#include "nsIPresShell.h"
#include "nsMemory.h"

using namespace mozilla;

int32_t nsFloatManager::sCachedFloatManagerCount = 0;
void* nsFloatManager::sCachedFloatManagers[NS_FLOAT_MANAGER_CACHE_SIZE];

/////////////////////////////////////////////////////////////////////////////
// nsFloatManager

nsFloatManager::nsFloatManager(nsIPresShell* aPresShell,
                               WritingMode aWM)
  :
#ifdef DEBUG
    mWritingMode(aWM),
#endif
    mLineLeft(0), mBlockStart(0),
    mFloatDamage(aPresShell),
    mPushedLeftFloatPastBreak(false),
    mPushedRightFloatPastBreak(false),
    mSplitLeftFloatAcrossBreak(false),
    mSplitRightFloatAcrossBreak(false)
{
  MOZ_COUNT_CTOR(nsFloatManager);
}

nsFloatManager::~nsFloatManager()
{
  MOZ_COUNT_DTOR(nsFloatManager);
}

// static
void* nsFloatManager::operator new(size_t aSize) CPP_THROW_NEW
{
  if (sCachedFloatManagerCount > 0) {
    // We have cached unused instances of this class, return a cached
    // instance in stead of always creating a new one.
    return sCachedFloatManagers[--sCachedFloatManagerCount];
  }

  // The cache is empty, this means we have to create a new instance using
  // the global |operator new|.
  return moz_xmalloc(aSize);
}

void
nsFloatManager::operator delete(void* aPtr, size_t aSize)
{
  if (!aPtr)
    return;
  // This float manager is no longer used, if there's still room in
  // the cache we'll cache this float manager, unless the layout
  // module was already shut down.

  if (sCachedFloatManagerCount < NS_FLOAT_MANAGER_CACHE_SIZE &&
      sCachedFloatManagerCount >= 0) {
    // There's still space in the cache for more instances, put this
    // instance in the cache in stead of deleting it.

    sCachedFloatManagers[sCachedFloatManagerCount++] = aPtr;
    return;
  }

  // The cache is full, or the layout module has been shut down,
  // delete this float manager.
  free(aPtr);
}


/* static */
void nsFloatManager::Shutdown()
{
  // The layout module is being shut down, clean up the cache and
  // disable further caching.

  int32_t i;

  for (i = 0; i < sCachedFloatManagerCount; i++) {
    void* floatManager = sCachedFloatManagers[i];
    if (floatManager)
      free(floatManager);
  }

  // Disable further caching.
  sCachedFloatManagerCount = -1;
}

#define CHECK_BLOCK_AND_LINE_DIR(aWM) \
  NS_ASSERTION((aWM).GetBlockDir() == mWritingMode.GetBlockDir() &&     \
               (aWM).IsLineInverted() == mWritingMode.IsLineInverted(), \
               "incompatible writing modes")

nsFlowAreaRect
nsFloatManager::GetFlowArea(WritingMode aWM, nscoord aBCoord, nscoord aBSize,
                            BandInfoType aBandInfoType, ShapeType aShapeType,
                            LogicalRect aContentArea, SavedState* aState,
                            const nsSize& aContainerSize) const
{
  CHECK_BLOCK_AND_LINE_DIR(aWM);
  NS_ASSERTION(aBSize >= 0, "unexpected max block size");
  NS_ASSERTION(aContentArea.ISize(aWM) >= 0,
               "unexpected content area inline size");

  nscoord blockStart = aBCoord + mBlockStart;
  if (blockStart < nscoord_MIN) {
    NS_WARNING("bad value");
    blockStart = nscoord_MIN;
  }

  // Determine the last float that we should consider.
  uint32_t floatCount;
  if (aState) {
    // Use the provided state.
    floatCount = aState->mFloatInfoCount;
    MOZ_ASSERT(floatCount <= mFloats.Length(), "bad state");
  } else {
    // Use our current state.
    floatCount = mFloats.Length();
  }

  // If there are no floats at all, or we're below the last one, return
  // quickly.
  if (floatCount == 0 ||
      (mFloats[floatCount-1].mLeftBEnd <= blockStart &&
       mFloats[floatCount-1].mRightBEnd <= blockStart)) {
    return nsFlowAreaRect(aWM, aContentArea.IStart(aWM), aBCoord,
                          aContentArea.ISize(aWM), aBSize, false);
  }

  nscoord blockEnd;
  if (aBSize == nscoord_MAX) {
    // This warning (and the two below) are possible to hit on pages
    // with really large objects.
    NS_WARNING_ASSERTION(aBandInfoType == BandInfoType::BandFromPoint, "bad height");
    blockEnd = nscoord_MAX;
  } else {
    blockEnd = blockStart + aBSize;
    if (blockEnd < blockStart || blockEnd > nscoord_MAX) {
      NS_WARNING("bad value");
      blockEnd = nscoord_MAX;
    }
  }
  nscoord lineLeft = mLineLeft + aContentArea.LineLeft(aWM, aContainerSize);
  nscoord lineRight = mLineLeft + aContentArea.LineRight(aWM, aContainerSize);
  if (lineRight < lineLeft) {
    NS_WARNING("bad value");
    lineRight = lineLeft;
  }

  // Walk backwards through the floats until we either hit the front of
  // the list or we're above |blockStart|.
  bool haveFloats = false;
  for (uint32_t i = floatCount; i > 0; --i) {
    const FloatInfo &fi = mFloats[i-1];
    if (fi.mLeftBEnd <= blockStart && fi.mRightBEnd <= blockStart) {
      // There aren't any more floats that could intersect this band.
      break;
    }
    if (fi.IsEmpty(aShapeType)) {
      // Ignore empty float areas.
      // https://drafts.csswg.org/css-shapes/#relation-to-box-model-and-float-behavior
      continue;
    }

    nscoord floatBStart = fi.BStart(aShapeType);
    nscoord floatBEnd = fi.BEnd(aShapeType);
    if (blockStart < floatBStart && aBandInfoType == BandInfoType::BandFromPoint) {
      // This float is below our band.  Shrink our band's height if needed.
      if (floatBStart < blockEnd) {
        blockEnd = floatBStart;
      }
    }
    // If blockStart == blockEnd (which happens only with WidthWithinHeight),
    // we include floats that begin at our 0-height vertical area.  We
    // need to do this to satisfy the invariant that a
    // WidthWithinHeight call is at least as narrow on both sides as a
    // BandFromPoint call beginning at its blockStart.
    else if (blockStart < floatBEnd &&
             (floatBStart < blockEnd ||
              (floatBStart == blockEnd && blockStart == blockEnd))) {
      // This float is in our band.

      // Shrink our band's width if needed.
      StyleFloat floatStyle = fi.mFrame->StyleDisplay()->PhysicalFloats(aWM);

      // When aBandInfoType is BandFromPoint, we're only intended to
      // consider a point along the y axis rather than a band.
      const nscoord bandBlockEnd =
        aBandInfoType == BandInfoType::BandFromPoint ? blockStart : blockEnd;
      if (floatStyle == StyleFloat::Left) {
        // A left float
        nscoord lineRightEdge =
          fi.LineRight(aShapeType, blockStart, bandBlockEnd);
        if (lineRightEdge > lineLeft) {
          lineLeft = lineRightEdge;
          // Only set haveFloats to true if the float is inside our
          // containing block.  This matches the spec for what some
          // callers want and disagrees for other callers, so we should
          // probably provide better information at some point.
          haveFloats = true;
        }
      } else {
        // A right float
        nscoord lineLeftEdge =
          fi.LineLeft(aShapeType, blockStart, bandBlockEnd);
        if (lineLeftEdge < lineRight) {
          lineRight = lineLeftEdge;
          // See above.
          haveFloats = true;
        }
      }

      // Shrink our band's height if needed.
      if (floatBEnd < blockEnd && aBandInfoType == BandInfoType::BandFromPoint) {
        blockEnd = floatBEnd;
      }
    }
  }

  nscoord blockSize = (blockEnd == nscoord_MAX) ?
                       nscoord_MAX : (blockEnd - blockStart);
  // convert back from LineLeft/Right to IStart
  nscoord inlineStart = aWM.IsBidiLTR()
                        ? lineLeft - mLineLeft
                        : mLineLeft - lineRight +
                          LogicalSize(aWM, aContainerSize).ISize(aWM);

  return nsFlowAreaRect(aWM, inlineStart, blockStart - mBlockStart,
                        lineRight - lineLeft, blockSize, haveFloats);
}

void
nsFloatManager::AddFloat(nsIFrame* aFloatFrame, const LogicalRect& aMarginRect,
                         WritingMode aWM, const nsSize& aContainerSize)
{
  CHECK_BLOCK_AND_LINE_DIR(aWM);
  NS_ASSERTION(aMarginRect.ISize(aWM) >= 0, "negative inline size!");
  NS_ASSERTION(aMarginRect.BSize(aWM) >= 0, "negative block size!");

  FloatInfo info(aFloatFrame, mLineLeft, mBlockStart, aMarginRect, aWM,
                 aContainerSize);

  // Set mLeftBEnd and mRightBEnd.
  if (HasAnyFloats()) {
    FloatInfo &tail = mFloats[mFloats.Length() - 1];
    info.mLeftBEnd = tail.mLeftBEnd;
    info.mRightBEnd = tail.mRightBEnd;
  } else {
    info.mLeftBEnd = nscoord_MIN;
    info.mRightBEnd = nscoord_MIN;
  }
  StyleFloat floatStyle = aFloatFrame->StyleDisplay()->PhysicalFloats(aWM);
  MOZ_ASSERT(floatStyle == StyleFloat::Left || floatStyle == StyleFloat::Right,
             "Unexpected float style!");
  nscoord& sideBEnd =
    floatStyle == StyleFloat::Left ? info.mLeftBEnd : info.mRightBEnd;
  nscoord thisBEnd = info.BEnd();
  if (thisBEnd > sideBEnd)
    sideBEnd = thisBEnd;

  mFloats.AppendElement(Move(info));
}

// static
LogicalRect
nsFloatManager::CalculateRegionFor(WritingMode          aWM,
                                   nsIFrame*            aFloat,
                                   const LogicalMargin& aMargin,
                                   const nsSize&        aContainerSize)
{
  // We consider relatively positioned frames at their original position.
  LogicalRect region(aWM, nsRect(aFloat->GetNormalPosition(),
                                 aFloat->GetSize()),
                     aContainerSize);

  // Float region includes its margin
  region.Inflate(aWM, aMargin);

  // Don't store rectangles with negative margin-box width or height in
  // the float manager; it can't deal with them.
  if (region.ISize(aWM) < 0) {
    // Preserve the right margin-edge for left floats and the left
    // margin-edge for right floats
    const nsStyleDisplay* display = aFloat->StyleDisplay();
    StyleFloat floatStyle = display->PhysicalFloats(aWM);
    if ((StyleFloat::Left == floatStyle) == aWM.IsBidiLTR()) {
      region.IStart(aWM) = region.IEnd(aWM);
    }
    region.ISize(aWM) = 0;
  }
  if (region.BSize(aWM) < 0) {
    region.BSize(aWM) = 0;
  }
  return region;
}

NS_DECLARE_FRAME_PROPERTY_DELETABLE(FloatRegionProperty, nsMargin)

LogicalRect
nsFloatManager::GetRegionFor(WritingMode aWM, nsIFrame* aFloat,
                             const nsSize& aContainerSize)
{
  LogicalRect region = aFloat->GetLogicalRect(aWM, aContainerSize);
  void* storedRegion = aFloat->GetProperty(FloatRegionProperty());
  if (storedRegion) {
    nsMargin margin = *static_cast<nsMargin*>(storedRegion);
    region.Inflate(aWM, LogicalMargin(aWM, margin));
  }
  return region;
}

void
nsFloatManager::StoreRegionFor(WritingMode aWM, nsIFrame* aFloat,
                               const LogicalRect& aRegion,
                               const nsSize& aContainerSize)
{
  nsRect region = aRegion.GetPhysicalRect(aWM, aContainerSize);
  nsRect rect = aFloat->GetRect();
  if (region.IsEqualEdges(rect)) {
    aFloat->DeleteProperty(FloatRegionProperty());
  }
  else {
    nsMargin* storedMargin = aFloat->GetProperty(FloatRegionProperty());
    if (!storedMargin) {
      storedMargin = new nsMargin();
      aFloat->SetProperty(FloatRegionProperty(), storedMargin);
    }
    *storedMargin = region - rect;
  }
}

nsresult
nsFloatManager::RemoveTrailingRegions(nsIFrame* aFrameList)
{
  if (!aFrameList) {
    return NS_OK;
  }
  // This could be a good bit simpler if we could guarantee that the
  // floats given were at the end of our list, so we could just search
  // for the head of aFrameList.  (But we can't;
  // layout/reftests/bugs/421710-1.html crashes.)
  nsTHashtable<nsPtrHashKey<nsIFrame> > frameSet(1);

  for (nsIFrame* f = aFrameList; f; f = f->GetNextSibling()) {
    frameSet.PutEntry(f);
  }

  uint32_t newLength = mFloats.Length();
  while (newLength > 0) {
    if (!frameSet.Contains(mFloats[newLength - 1].mFrame)) {
      break;
    }
    --newLength;
  }
  mFloats.TruncateLength(newLength);

#ifdef DEBUG
  for (uint32_t i = 0; i < mFloats.Length(); ++i) {
    NS_ASSERTION(!frameSet.Contains(mFloats[i].mFrame),
                 "Frame region deletion was requested but we couldn't delete it");
  }
#endif

  return NS_OK;
}

void
nsFloatManager::PushState(SavedState* aState)
{
  NS_PRECONDITION(aState, "Need a place to save state");

  // This is a cheap push implementation, which
  // only saves the (x,y) and last frame in the mFrameInfoMap
  // which is enough info to get us back to where we should be
  // when pop is called.
  //
  // This push/pop mechanism is used to undo any
  // floats that were added during the unconstrained reflow
  // in nsBlockReflowContext::DoReflowBlock(). (See bug 96736)
  //
  // It should also be noted that the state for mFloatDamage is
  // intentionally not saved or restored in PushState() and PopState(),
  // since that could lead to bugs where damage is missed/dropped when
  // we move from position A to B (during the intermediate incremental
  // reflow mentioned above) and then from B to C during the subsequent
  // reflow. In the typical case A and C will be the same, but not always.
  // Allowing mFloatDamage to accumulate the damage incurred during both
  // reflows ensures that nothing gets missed.
  aState->mLineLeft = mLineLeft;
  aState->mBlockStart = mBlockStart;
  aState->mPushedLeftFloatPastBreak = mPushedLeftFloatPastBreak;
  aState->mPushedRightFloatPastBreak = mPushedRightFloatPastBreak;
  aState->mSplitLeftFloatAcrossBreak = mSplitLeftFloatAcrossBreak;
  aState->mSplitRightFloatAcrossBreak = mSplitRightFloatAcrossBreak;
  aState->mFloatInfoCount = mFloats.Length();
}

void
nsFloatManager::PopState(SavedState* aState)
{
  NS_PRECONDITION(aState, "No state to restore?");

  mLineLeft = aState->mLineLeft;
  mBlockStart = aState->mBlockStart;
  mPushedLeftFloatPastBreak = aState->mPushedLeftFloatPastBreak;
  mPushedRightFloatPastBreak = aState->mPushedRightFloatPastBreak;
  mSplitLeftFloatAcrossBreak = aState->mSplitLeftFloatAcrossBreak;
  mSplitRightFloatAcrossBreak = aState->mSplitRightFloatAcrossBreak;

  NS_ASSERTION(aState->mFloatInfoCount <= mFloats.Length(),
               "somebody misused PushState/PopState");
  mFloats.TruncateLength(aState->mFloatInfoCount);
}

nscoord
nsFloatManager::GetLowestFloatTop() const
{
  if (mPushedLeftFloatPastBreak || mPushedRightFloatPastBreak) {
    return nscoord_MAX;
  }
  if (!HasAnyFloats()) {
    return nscoord_MIN;
  }
  return mFloats[mFloats.Length() -1].BStart() - mBlockStart;
}

#ifdef DEBUG_FRAME_DUMP
void
DebugListFloatManager(const nsFloatManager *aFloatManager)
{
  aFloatManager->List(stdout);
}

nsresult
nsFloatManager::List(FILE* out) const
{
  if (!HasAnyFloats())
    return NS_OK;

  for (uint32_t i = 0; i < mFloats.Length(); ++i) {
    const FloatInfo &fi = mFloats[i];
    fprintf_stderr(out, "Float %u: frame=%p rect={%d,%d,%d,%d} BEnd={l:%d, r:%d}\n",
                   i, static_cast<void*>(fi.mFrame),
                   fi.LineLeft(), fi.BStart(), fi.ISize(), fi.BSize(),
                   fi.mLeftBEnd, fi.mRightBEnd);
  }
  return NS_OK;
}
#endif

nscoord
nsFloatManager::ClearFloats(nscoord aBCoord, StyleClear aBreakType,
                            uint32_t aFlags) const
{
  if (!(aFlags & DONT_CLEAR_PUSHED_FLOATS) && ClearContinues(aBreakType)) {
    return nscoord_MAX;
  }
  if (!HasAnyFloats()) {
    return aBCoord;
  }

  nscoord blockEnd = aBCoord + mBlockStart;

  const FloatInfo &tail = mFloats[mFloats.Length() - 1];
  switch (aBreakType) {
    case StyleClear::Both:
      blockEnd = std::max(blockEnd, tail.mLeftBEnd);
      blockEnd = std::max(blockEnd, tail.mRightBEnd);
      break;
    case StyleClear::Left:
      blockEnd = std::max(blockEnd, tail.mLeftBEnd);
      break;
    case StyleClear::Right:
      blockEnd = std::max(blockEnd, tail.mRightBEnd);
      break;
    default:
      // Do nothing
      break;
  }

  blockEnd -= mBlockStart;

  return blockEnd;
}

bool
nsFloatManager::ClearContinues(StyleClear aBreakType) const
{
  return ((mPushedLeftFloatPastBreak || mSplitLeftFloatAcrossBreak) &&
          (aBreakType == StyleClear::Both ||
           aBreakType == StyleClear::Left)) ||
         ((mPushedRightFloatPastBreak || mSplitRightFloatAcrossBreak) &&
          (aBreakType == StyleClear::Both ||
           aBreakType == StyleClear::Right));
}

/////////////////////////////////////////////////////////////////////////////
// ShapeInfo is an abstract class for implementing all the shapes in CSS
// Shapes Module. A subclass needs to override all the methods to adjust
// the flow area with respect to its shape.
//
class nsFloatManager::ShapeInfo
{
public:
  virtual ~ShapeInfo() {}

  virtual nscoord LineLeft(const nscoord aBStart,
                           const nscoord aBEnd) const = 0;
  virtual nscoord LineRight(const nscoord aBStart,
                            const nscoord aBEnd) const = 0;
  virtual nscoord BStart() const = 0;
  virtual nscoord BEnd() const = 0;
  virtual bool IsEmpty() const = 0;

  // Translate the current origin by the specified offsets.
  virtual void Translate(nscoord aLineLeft, nscoord aBlockStart) = 0;

  static LogicalRect ComputeShapeBoxRect(
    const StyleShapeSource& aShapeOutside,
    nsIFrame* const aFrame,
    const LogicalRect& aMarginRect,
    WritingMode aWM);

  // Convert the LogicalRect to the special logical coordinate space used
  // in float manager.
  static nsRect ConvertToFloatLogical(const LogicalRect& aRect,
                                      WritingMode aWM,
                                      const nsSize& aContainerSize)
  {
    return nsRect(aRect.LineLeft(aWM, aContainerSize), aRect.BStart(aWM),
                  aRect.ISize(aWM), aRect.BSize(aWM));
  }

  static UniquePtr<ShapeInfo> CreateShapeBox(
    nsIFrame* const aFrame,
    const LogicalRect& aShapeBoxRect,
    WritingMode aWM,
    const nsSize& aContainerSize);

  static UniquePtr<ShapeInfo> CreateBasicShape(
    const UniquePtr<StyleBasicShape>& aBasicShape,
    const LogicalRect& aShapeBoxRect,
    WritingMode aWM,
    const nsSize& aContainerSize);

  static UniquePtr<ShapeInfo> CreateInset(
    const UniquePtr<StyleBasicShape>& aBasicShape,
    const LogicalRect& aShapeBoxRect,
    WritingMode aWM,
    const nsSize& aContainerSize);

  static UniquePtr<ShapeInfo> CreateCircleOrEllipse(
    const UniquePtr<StyleBasicShape>& aBasicShape,
    const LogicalRect& aShapeBoxRect,
    WritingMode aWM,
    const nsSize& aContainerSize);

  static UniquePtr<ShapeInfo> CreatePolygon(
    const UniquePtr<StyleBasicShape>& aBasicShape,
    const LogicalRect& aShapeBoxRect,
    WritingMode aWM,
    const nsSize& aContainerSize);

  static UniquePtr<ShapeInfo> CreateImageShape(
    const UniquePtr<nsStyleImage>& aShapeImage,
    float aShapeImageThreshold,
    nsIFrame* const aFrame,
    WritingMode aWM,
    const nsSize& aContainerSize);

protected:
  // Compute the minimum line-axis difference between the bounding shape
  // box and its rounded corner within the given band (block-axis region).
  // This is used as a helper function to compute the LineRight() and
  // LineLeft(). See the picture in the implementation for an example.
  // RadiusL and RadiusB stand for radius on the line-axis and block-axis.
  //
  // Returns radius-x diff on the line-axis, or 0 if there's no rounded
  // corner within the given band.
  static nscoord ComputeEllipseLineInterceptDiff(
    const nscoord aShapeBoxBStart, const nscoord aShapeBoxBEnd,
    const nscoord aBStartCornerRadiusL, const nscoord aBStartCornerRadiusB,
    const nscoord aBEndCornerRadiusL, const nscoord aBEndCornerRadiusB,
    const nscoord aBandBStart, const nscoord aBandBEnd);

  static nscoord XInterceptAtY(const nscoord aY, const nscoord aRadiusX,
                               const nscoord aRadiusY);

  // Convert the physical point to the special logical coordinate space
  // used in float manager.
  static nsPoint ConvertToFloatLogical(const nsPoint& aPoint,
                                       WritingMode aWM,
                                       const nsSize& aContainerSize);

  // Convert the half corner radii (nscoord[8]) to the special logical
  // coordinate space used in float manager.
  static UniquePtr<nscoord[]> ConvertToFloatLogical(
    const nscoord aRadii[8],
    WritingMode aWM);
};

/////////////////////////////////////////////////////////////////////////////
// RoundedBoxShapeInfo
//
// Implements shape-outside: <shape-box> and shape-outside: inset().
//
class nsFloatManager::RoundedBoxShapeInfo final : public nsFloatManager::ShapeInfo
{
public:
  RoundedBoxShapeInfo(const nsRect& aRect,
                      UniquePtr<nscoord[]> aRadii)
    : mRect(aRect)
    , mRadii(Move(aRadii))
  {}

  nscoord LineLeft(const nscoord aBStart,
                   const nscoord aBEnd) const override;
  nscoord LineRight(const nscoord aBStart,
                    const nscoord aBEnd) const override;
  nscoord BStart() const override { return mRect.y; }
  nscoord BEnd() const override { return mRect.YMost(); }
  bool IsEmpty() const override { return mRect.IsEmpty(); };

  void Translate(nscoord aLineLeft, nscoord aBlockStart) override
  {
    mRect.MoveBy(aLineLeft, aBlockStart);
  }

private:
  // The rect of the rounded box shape in the float manager's coordinate
  // space.
  nsRect mRect;
  // The half corner radii of the reference box. It's an nscoord[8] array
  // in the float manager's coordinate space. If there are no radii, it's
  // nullptr.
  UniquePtr<nscoord[]> mRadii;
};

nscoord
nsFloatManager::RoundedBoxShapeInfo::LineLeft(const nscoord aBStart,
                                              const nscoord aBEnd) const
{
  if (!mRadii) {
    return mRect.x;
  }

  nscoord lineLeftDiff =
    ComputeEllipseLineInterceptDiff(
      mRect.y, mRect.YMost(),
      mRadii[eCornerTopLeftX], mRadii[eCornerTopLeftY],
      mRadii[eCornerBottomLeftX], mRadii[eCornerBottomLeftY],
      aBStart, aBEnd);
  return mRect.x + lineLeftDiff;
}

nscoord
nsFloatManager::RoundedBoxShapeInfo::LineRight(const nscoord aBStart,
                                               const nscoord aBEnd) const
{
  if (!mRadii) {
    return mRect.XMost();
  }

  nscoord lineRightDiff =
    ComputeEllipseLineInterceptDiff(
      mRect.y, mRect.YMost(),
      mRadii[eCornerTopRightX], mRadii[eCornerTopRightY],
      mRadii[eCornerBottomRightX], mRadii[eCornerBottomRightY],
      aBStart, aBEnd);
  return mRect.XMost() - lineRightDiff;
}

/////////////////////////////////////////////////////////////////////////////
// EllipseShapeInfo
//
// Implements shape-outside: circle() and shape-outside: ellipse().
//
class nsFloatManager::EllipseShapeInfo final : public nsFloatManager::ShapeInfo
{
public:
  EllipseShapeInfo(const nsPoint& aCenter,
                   const nsSize& aRadii)
    : mCenter(aCenter)
    , mRadii(aRadii)
  {}

  nscoord LineLeft(const nscoord aBStart,
                   const nscoord aBEnd) const override;
  nscoord LineRight(const nscoord aBStart,
                    const nscoord aBEnd) const override;
  nscoord BStart() const override { return mCenter.y - mRadii.height; }
  nscoord BEnd() const override { return mCenter.y + mRadii.height; }
  bool IsEmpty() const override { return mRadii.IsEmpty(); };

  void Translate(nscoord aLineLeft, nscoord aBlockStart) override
  {
    mCenter.MoveBy(aLineLeft, aBlockStart);
  }

private:
  // The position of the center of the ellipse. The coordinate space is the
  // same as FloatInfo::mRect.
  nsPoint mCenter;
  // The radii of the ellipse in app units. The width and height represent
  // the line-axis and block-axis radii of the ellipse.
  nsSize mRadii;
};

nscoord
nsFloatManager::EllipseShapeInfo::LineLeft(const nscoord aBStart,
                                           const nscoord aBEnd) const
{
  nscoord lineLeftDiff =
    ComputeEllipseLineInterceptDiff(BStart(), BEnd(),
                                    mRadii.width, mRadii.height,
                                    mRadii.width, mRadii.height,
                                    aBStart, aBEnd);
  return mCenter.x - mRadii.width + lineLeftDiff;
}

nscoord
nsFloatManager::EllipseShapeInfo::LineRight(const nscoord aBStart,
                                            const nscoord aBEnd) const
{
  nscoord lineRightDiff =
    ComputeEllipseLineInterceptDiff(BStart(), BEnd(),
                                    mRadii.width, mRadii.height,
                                    mRadii.width, mRadii.height,
                                    aBStart, aBEnd);
  return mCenter.x + mRadii.width - lineRightDiff;
}

/////////////////////////////////////////////////////////////////////////////
// PolygonShapeInfo
//
// Implements shape-outside: polygon().
//
class nsFloatManager::PolygonShapeInfo final : public nsFloatManager::ShapeInfo
{
public:
  explicit PolygonShapeInfo(nsTArray<nsPoint>&& aVertices);

  nscoord LineLeft(const nscoord aBStart,
                   const nscoord aBEnd) const override;
  nscoord LineRight(const nscoord aBStart,
                    const nscoord aBEnd) const override;
  nscoord BStart() const override { return mBStart; }
  nscoord BEnd() const override { return mBEnd; }
  bool IsEmpty() const override { return mEmpty; }

  void Translate(nscoord aLineLeft, nscoord aBlockStart) override;

private:
  // Helper method for implementing LineLeft() and LineRight().
  nscoord ComputeLineIntercept(
    const nscoord aBStart,
    const nscoord aBEnd,
    nscoord (*aCompareOp) (std::initializer_list<nscoord>),
    const nscoord aLineInterceptInitialValue) const;

  // Given a horizontal line y, and two points p1 and p2 forming a line
  // segment L. Solve x for the intersection of y and L. This method
  // assumes y and L do intersect, and L is *not* horizontal.
  static nscoord XInterceptAtY(const nscoord aY,
                               const nsPoint& aP1,
                               const nsPoint& aP2);

  // The vertices of the polygon in the float manager's coordinate space.
  nsTArray<nsPoint> mVertices;

  // If mEmpty is true, that means the polygon encloses no area.
  bool mEmpty = false;

  // Computed block start and block end value of the polygon shape.
  //
  // If mEmpty is false, their initial values nscoord_MAX and nscoord_MIN
  // are used as sentinels for computing min() and max() in the
  // constructor, and mBStart is guaranteed to be less than or equal to
  // mBEnd. If mEmpty is true, their values do not matter.
  nscoord mBStart = nscoord_MAX;
  nscoord mBEnd = nscoord_MIN;
};

nsFloatManager::PolygonShapeInfo::PolygonShapeInfo(nsTArray<nsPoint>&& aVertices)
  : mVertices(aVertices)
{
  // Polygons with fewer than three vertices result in an empty area.
  // https://drafts.csswg.org/css-shapes/#funcdef-polygon
  if (mVertices.Length() < 3) {
    mEmpty = true;
    return;
  }

  auto Determinant = [] (const nsPoint& aP0, const nsPoint& aP1) {
    // Returns the determinant of the 2x2 matrix [aP0 aP1].
    // https://en.wikipedia.org/wiki/Determinant#2_.C3.97_2_matrices
    return aP0.x * aP1.y - aP0.y * aP1.x;
  };

  // See if we have any vertices that are non-collinear with the first two.
  // (If a polygon's vertices are all collinear, it encloses no area.)
  bool isEntirelyCollinear = true;
  const nsPoint& p0 = mVertices[0];
  const nsPoint& p1 = mVertices[1];
  for (size_t i = 2; i < mVertices.Length(); ++i) {
    const nsPoint& p2 = mVertices[i];

    // If the determinant of the matrix formed by two points is 0, that
    // means they're collinear with respect to the origin. Here, if it's
    // nonzero, then p1 and p2 are non-collinear with respect to p0, i.e.
    // the three points are non-collinear.
    if (Determinant(p2 - p0, p1 - p0) != 0) {
      isEntirelyCollinear = false;
      break;
    }
  }

  if (isEntirelyCollinear) {
    mEmpty = true;
    return;
  }

  // mBStart and mBEnd are the lower and the upper bounds of all the
  // vertex.y, respectively. The vertex.y is actually on the block-axis of
  // the float manager's writing mode.
  for (const nsPoint& vertex : mVertices) {
    mBStart = std::min(mBStart, vertex.y);
    mBEnd = std::max(mBEnd, vertex.y);
  }
}

nscoord
nsFloatManager::PolygonShapeInfo::LineLeft(const nscoord aBStart,
                                           const nscoord aBEnd) const
{
  MOZ_ASSERT(!mEmpty, "Shouldn't be called if the polygon encloses no area.");

  // We want the line-left-most inline-axis coordinate where the
  // (block-axis) aBStart/aBEnd band crosses a line segment of the polygon.
  // To get that, we start as line-right as possible (at nscoord_MAX). Then
  // we iterate each line segment to compute its intersection point with the
  // band (if any) and using std::min() successively to get the smallest
  // inline-coordinates among those intersection points.
  //
  // Note: std::min<nscoord> means the function std::min() with template
  // parameter nscoord, not the minimum value of nscoord.
  return ComputeLineIntercept(aBStart, aBEnd, std::min<nscoord>, nscoord_MAX);
}

nscoord
nsFloatManager::PolygonShapeInfo::LineRight(const nscoord aBStart,
                                            const nscoord aBEnd) const
{
  MOZ_ASSERT(!mEmpty, "Shouldn't be called if the polygon encloses no area.");

  // Similar to LineLeft(). Though here, we want the line-right-most
  // inline-axis coordinate, so we instead start at nscoord_MIN and use
  // std::max() to get the biggest inline-coordinate among those
  // intersection points.
  return ComputeLineIntercept(aBStart, aBEnd, std::max<nscoord>, nscoord_MIN);
}

nscoord
nsFloatManager::PolygonShapeInfo::ComputeLineIntercept(
  const nscoord aBStart,
  const nscoord aBEnd,
  nscoord (*aCompareOp) (std::initializer_list<nscoord>),
  const nscoord aLineInterceptInitialValue) const
{
  MOZ_ASSERT(aBStart <= aBEnd,
             "The band's block start is greater than its block end?");

  const size_t len = mVertices.Length();
  nscoord lineIntercept = aLineInterceptInitialValue;

  // Iterate each line segment {p0, p1}, {p1, p2}, ..., {pn, p0}.
  for (size_t i = 0; i < len; ++i) {
    const nsPoint* smallYVertex = &mVertices[i];
    const nsPoint* bigYVertex = &mVertices[(i + 1) % len];

    // Swap the two points to satisfy the requirement for calling
    // XInterceptAtY.
    if (smallYVertex->y > bigYVertex->y) {
      std::swap(smallYVertex, bigYVertex);
    }

    if (aBStart >= bigYVertex->y || aBEnd <= smallYVertex->y ||
        smallYVertex->y == bigYVertex->y) {
      // Skip computing the intercept if a) the band doesn't intersect the
      // line segment (even if it crosses one of two the vertices); or b)
      // the line segment is horizontal. It's OK because the two end points
      // forming this horizontal segment will still be considered if each of
      // them is forming another non-horizontal segment with other points.
      continue;
    }

    nscoord bStartLineIntercept =
      aBStart <= smallYVertex->y
        ? smallYVertex->x
        : XInterceptAtY(aBStart, *smallYVertex, *bigYVertex);
    nscoord bEndLineIntercept =
      aBEnd >= bigYVertex->y
        ? bigYVertex->x
        : XInterceptAtY(aBEnd, *smallYVertex, *bigYVertex);

    // If either new intercept is more extreme than lineIntercept (per
    // aCompareOp), then update lineIntercept to that value.
    lineIntercept =
      aCompareOp({lineIntercept, bStartLineIntercept, bEndLineIntercept});
  }

  return lineIntercept;
}

void
nsFloatManager::PolygonShapeInfo::Translate(nscoord aLineLeft,
                                            nscoord aBlockStart)
{
  for (nsPoint& vertex : mVertices) {
    vertex.MoveBy(aLineLeft, aBlockStart);
  }
  mBStart += aBlockStart;
  mBEnd += aBlockStart;
}

/* static */ nscoord
nsFloatManager::PolygonShapeInfo::XInterceptAtY(const nscoord aY,
                                                const nsPoint& aP1,
                                                const nsPoint& aP2)
{
  // Solve for x in the linear equation: x = x1 + (y-y1) * (x2-x1) / (y2-y1),
  // where aP1 = (x1, y1) and aP2 = (x2, y2).

  MOZ_ASSERT(aP1.y <= aY && aY <= aP2.y,
             "This function won't work if the horizontal line at aY and "
             "the line segment (aP1, aP2) do not intersect!");

  MOZ_ASSERT(aP1.y != aP2.y,
             "A horizontal line segment results in dividing by zero error!");

  return aP1.x + (aY - aP1.y) * (aP2.x - aP1.x) / (aP2.y - aP1.y);
}

/////////////////////////////////////////////////////////////////////////////
// ImageShapeInfo
//
// Implements shape-outside: <image>
//
class nsFloatManager::ImageShapeInfo final : public nsFloatManager::ShapeInfo
{
public:
  ImageShapeInfo(uint8_t* aAlphaPixels,
                 int32_t aStride,
                 const LayoutDeviceIntSize& aImageSize,
                 int32_t aAppUnitsPerDevPixel,
                 float aShapeImageThreshold,
                 const nsRect& aContentRect,
                 WritingMode aWM,
                 const nsSize& aContainerSize);

  nscoord LineLeft(const nscoord aBStart,
                   const nscoord aBEnd) const override;
  nscoord LineRight(const nscoord aBStart,
                    const nscoord aBEnd) const override;
  nscoord BStart() const override { return mBStart; }
  nscoord BEnd() const override { return mBEnd; }
  bool IsEmpty() const override { return mIntervals.IsEmpty(); }

  void Translate(nscoord aLineLeft, nscoord aBlockStart) override;

private:
  size_t MinIntervalIndexContainingY(const nscoord aTargetY) const;
  nscoord LineEdge(const nscoord aBStart,
                   const nscoord aBEnd,
                   bool aLeft) const;

  // An interval is slice of the float area defined by this ImageShapeInfo.
  // Each interval is a rectangle that is one pixel deep in the block
  // axis. The values are stored as block edges in the y coordinates,
  // and inline edges as the x coordinates.

  // The intervals are stored in ascending order on y.
  nsTArray<nsRect> mIntervals;

  nscoord mBStart = nscoord_MAX;
  nscoord mBEnd = nscoord_MIN;
};

nsFloatManager::ImageShapeInfo::ImageShapeInfo(
  uint8_t* aAlphaPixels,
  int32_t aStride,
  const LayoutDeviceIntSize& aImageSize,
  int32_t aAppUnitsPerDevPixel,
  float aShapeImageThreshold,
  const nsRect& aContentRect,
  WritingMode aWM,
  const nsSize& aContainerSize)
{
  MOZ_ASSERT(aShapeImageThreshold >=0.0 && aShapeImageThreshold <=1.0,
             "The computed value of shape-image-threshold is wrong!");

  const uint8_t threshold = NSToIntFloor(aShapeImageThreshold * 255);
  const int32_t w = aImageSize.width;
  const int32_t h = aImageSize.height;

  // Scan the pixels in a double loop. For horizontal writing modes, we do
  // this row by row, from top to bottom. For vertical writing modes, we do
  // column by column, from left to right. We define the two loops
  // generically, then figure out the rows and cols within the i loop.
  const int32_t bSize = aWM.IsVertical() ? w : h;
  const int32_t iSize = aWM.IsVertical() ? h : w;
  for (int32_t b = 0; b < bSize; ++b) {
    // iMin and iMax store the start and end of the float area for the row
    // or column represented by this iteration of the b loop.
    int32_t iMin = -1;
    int32_t iMax = -1;

    for (int32_t i = 0; i < iSize; ++i) {
      const int32_t col = aWM.IsVertical() ? b : i;
      const int32_t row = aWM.IsVertical() ? i : b;

      // Determine if the alpha pixel at this row and column has a value
      // greater than the threshold. If it does, update our iMin and iMax values
      // to track the edges of the float area for this row or column.
      // https://drafts.csswg.org/css-shapes-1/#valdef-shape-image-threshold-number
      const uint8_t alpha = aAlphaPixels[col + row * aStride];
      if (alpha > threshold) {
        if (iMin == -1) {
          iMin = i;
        }
        MOZ_ASSERT(iMax < i);
        iMax = i;
      }
    }

    // At the end of a row or column; did we find something?
    if (iMin != -1) {
      // Store an interval as an nsRect with our inline axis values stored in x
      // and our block axis values stored in y. The position is dependent on
      // the writing mode, but the size is the same for all writing modes.

      // Size is the difference in inline axis edges stored as x, and one
      // block axis pixel stored as y. For the inline axis, we add 1 to iMax
      // because we want to capture the far edge of the last pixel.
      nsSize size(((iMax + 1) - iMin) * aAppUnitsPerDevPixel,
                  aAppUnitsPerDevPixel);

      // Since we started our scanning of the image pixels from the top left,
      // the interval position starts from the origin of the content rect,
      // converted to logical coordinates.
      nsPoint origin = ConvertToFloatLogical(aContentRect.TopLeft(), aWM,
                                             aContainerSize);

      // Depending on the writing mode, we now move the origin.
      if (aWM.IsVerticalRL()) {
        // vertical-rl or sideways-rl.
        // These writing modes proceed from the top right, and each interval
        // moves in a positive inline direction and negative block direction.
        // That means that the intervals will be reversed after all have been
        // constructed. We add 1 to b to capture the end of the block axis pixel.
        origin.MoveBy(iMin * aAppUnitsPerDevPixel, (b + 1) * -aAppUnitsPerDevPixel);
      } else if (aWM.IsVerticalLR() && aWM.IsSideways()) {
        // sideways-lr.
        // These writing modes proceed from the bottom left, and each interval
        // moves in a negative inline direction and a positive block direction.
        // We add 1 to iMax to capture the end of the inline axis pixel.
        origin.MoveBy((iMax + 1) * -aAppUnitsPerDevPixel, b * aAppUnitsPerDevPixel);
      } else {
        // horizontal-tb or vertical-lr.
        // These writing modes proceed from the top left and each interval
        // moves in a positive step in both inline and block directions.
        origin.MoveBy(iMin * aAppUnitsPerDevPixel, b * aAppUnitsPerDevPixel);
      }

      mIntervals.AppendElement(nsRect(origin, size));
    }
  }

  if (aWM.IsVerticalRL()) {
    // vertical-rl or sideways-rl.
    // Because we scan the columns from left to right, we need to reverse
    // the array so that it's sorted (in ascending order) on the block
    // direction.
    mIntervals.Reverse();
  }

  if (!mIntervals.IsEmpty()) {
    mBStart = mIntervals[0].Y();
    mBEnd = mIntervals.LastElement().YMost();
  }
}

size_t
nsFloatManager::ImageShapeInfo::MinIntervalIndexContainingY(
  const nscoord aTargetY) const
{
  // Perform a binary search to find the minimum index of an interval
  // that contains aTargetY. If no such interval exists, return a value
  // equal to the number of intervals.
  size_t startIdx = 0;
  size_t endIdx = mIntervals.Length();
  while (startIdx < endIdx) {
    size_t midIdx = startIdx + (endIdx - startIdx) / 2;
    if (mIntervals[midIdx].ContainsY(aTargetY)) {
      return midIdx;
    }
    nscoord midY = mIntervals[midIdx].Y();
    if (midY < aTargetY) {
      startIdx = midIdx + 1;
    } else {
      endIdx = midIdx;
    }
  }

  return endIdx;
}

nscoord
nsFloatManager::ImageShapeInfo::LineEdge(const nscoord aBStart,
                                         const nscoord aBEnd,
                                         bool aLeft) const
{
  MOZ_ASSERT(aBStart <= aBEnd,
             "The band's block start is greater than its block end?");

  // Find all the intervals whose rects overlap the aBStart to
  // aBEnd range, and find the most constraining inline edge
  // depending on the value of aLeft.

  // Since the intervals are stored in block-axis order, we need
  // to find the first interval that overlaps aBStart and check
  // succeeding intervals until we get past aBEnd.

  nscoord lineEdge = aLeft ? nscoord_MAX : nscoord_MIN;

  size_t intervalCount = mIntervals.Length();
  for (size_t i = MinIntervalIndexContainingY(aBStart);
	   i < intervalCount; ++i) {
    // We can always get the bCoord from the intervals' mLineLeft,
    // since the y() coordinate is duplicated in both points in the
    // interval.
    auto& interval = mIntervals[i];
    nscoord bCoord = interval.Y();
    if (bCoord > aBEnd) {
      break;
    }
    // Get the edge from the interval point indicated by aLeft.
    if (aLeft) {
      lineEdge = std::min(lineEdge, interval.X());
    } else {
      lineEdge = std::max(lineEdge, interval.XMost());
    }
  }

  return lineEdge;
}

nscoord
nsFloatManager::ImageShapeInfo::LineLeft(const nscoord aBStart,
                                         const nscoord aBEnd) const
{
  return LineEdge(aBStart, aBEnd, true);
}

nscoord
nsFloatManager::ImageShapeInfo::LineRight(const nscoord aBStart,
                                          const nscoord aBEnd) const
{
  return LineEdge(aBStart, aBEnd, false);
}

void
nsFloatManager::ImageShapeInfo::Translate(nscoord aLineLeft,
                                          nscoord aBlockStart)
{
  for (nsRect& interval : mIntervals) {
    interval.MoveBy(aLineLeft, aBlockStart);
  }

  mBStart += aBlockStart;
  mBEnd += aBlockStart;
}

/////////////////////////////////////////////////////////////////////////////
// FloatInfo

nsFloatManager::FloatInfo::FloatInfo(nsIFrame* aFrame,
                                     nscoord aLineLeft, nscoord aBlockStart,
                                     const LogicalRect& aMarginRect,
                                     WritingMode aWM,
                                     const nsSize& aContainerSize)
  : mFrame(aFrame)
  , mRect(ShapeInfo::ConvertToFloatLogical(aMarginRect, aWM, aContainerSize) +
          nsPoint(aLineLeft, aBlockStart))
{
  MOZ_COUNT_CTOR(nsFloatManager::FloatInfo);

  if (IsEmpty()) {
    // Per spec, a float area defined by a shape is clipped to the float’s
    // margin box. Therefore, no need to create a shape info if the float's
    // margin box is empty, since a float area can only be smaller than the
    // margin box.

    // https://drafts.csswg.org/css-shapes/#relation-to-box-model-and-float-behavior
    return;
  }

  const StyleShapeSource& shapeOutside = mFrame->StyleDisplay()->mShapeOutside;

  switch (shapeOutside.GetType()) {
    case StyleShapeSourceType::None:
      // No need to create shape info.
      return;

    case StyleShapeSourceType::URL:
      MOZ_ASSERT_UNREACHABLE("shape-outside doesn't have URL source type!");
      return;

    case StyleShapeSourceType::Image: {
      float shapeImageThreshold = mFrame->StyleDisplay()->mShapeImageThreshold;
      mShapeInfo = ShapeInfo::CreateImageShape(shapeOutside.GetShapeImage(),
                                               shapeImageThreshold,
                                               mFrame,
                                               aWM,
                                               aContainerSize);
      if (!mShapeInfo) {
        // Image is not ready, or fails to load, etc.
        return;
      }

      break;
    }

    case StyleShapeSourceType::Box: {
      // Initialize <shape-box>'s reference rect.
      LogicalRect shapeBoxRect =
        ShapeInfo::ComputeShapeBoxRect(shapeOutside, mFrame, aMarginRect, aWM);
      mShapeInfo = ShapeInfo::CreateShapeBox(mFrame, shapeBoxRect, aWM,
                                             aContainerSize);
      break;
    }

    case StyleShapeSourceType::Shape: {
      const UniquePtr<StyleBasicShape>& basicShape = shapeOutside.GetBasicShape();
      // Initialize <shape-box>'s reference rect.
      LogicalRect shapeBoxRect =
        ShapeInfo::ComputeShapeBoxRect(shapeOutside, mFrame, aMarginRect, aWM);
      mShapeInfo = ShapeInfo::CreateBasicShape(basicShape, shapeBoxRect, aWM,
                                               aContainerSize);
      break;
    }
  }

  MOZ_ASSERT(mShapeInfo,
             "All shape-outside values except none should have mShapeInfo!");

  // Translate the shape to the same origin as nsFloatManager.
  mShapeInfo->Translate(aLineLeft, aBlockStart);
}

#ifdef NS_BUILD_REFCNT_LOGGING
nsFloatManager::FloatInfo::FloatInfo(FloatInfo&& aOther)
  : mFrame(Move(aOther.mFrame))
  , mLeftBEnd(Move(aOther.mLeftBEnd))
  , mRightBEnd(Move(aOther.mRightBEnd))
  , mRect(Move(aOther.mRect))
  , mShapeInfo(Move(aOther.mShapeInfo))
{
  MOZ_COUNT_CTOR(nsFloatManager::FloatInfo);
}

nsFloatManager::FloatInfo::~FloatInfo()
{
  MOZ_COUNT_DTOR(nsFloatManager::FloatInfo);
}
#endif

nscoord
nsFloatManager::FloatInfo::LineLeft(ShapeType aShapeType,
                                    const nscoord aBStart,
                                    const nscoord aBEnd) const
{
  if (aShapeType == ShapeType::Margin) {
    return LineLeft();
  }

  MOZ_ASSERT(aShapeType == ShapeType::ShapeOutside);
  if (!mShapeInfo) {
    return LineLeft();
  }
  // Clip the flow area to the margin-box because
  // https://drafts.csswg.org/css-shapes-1/#relation-to-box-model-and-float-behavior
  // says "When a shape is used to define a float area, the shape is clipped
  // to the float’s margin box."
  return std::max(LineLeft(), mShapeInfo->LineLeft(aBStart, aBEnd));
}

nscoord
nsFloatManager::FloatInfo::LineRight(ShapeType aShapeType,
                                     const nscoord aBStart,
                                     const nscoord aBEnd) const
{
  if (aShapeType == ShapeType::Margin) {
    return LineRight();
  }

  MOZ_ASSERT(aShapeType == ShapeType::ShapeOutside);
  if (!mShapeInfo) {
    return LineRight();
  }
  // Clip the flow area to the margin-box. See LineLeft().
  return std::min(LineRight(), mShapeInfo->LineRight(aBStart, aBEnd));
}

nscoord
nsFloatManager::FloatInfo::BStart(ShapeType aShapeType) const
{
  if (aShapeType == ShapeType::Margin) {
    return BStart();
  }

  MOZ_ASSERT(aShapeType == ShapeType::ShapeOutside);
  if (!mShapeInfo) {
    return BStart();
  }
  // Clip the flow area to the margin-box. See LineLeft().
  return std::max(BStart(), mShapeInfo->BStart());
}

nscoord
nsFloatManager::FloatInfo::BEnd(ShapeType aShapeType) const
{
  if (aShapeType == ShapeType::Margin) {
    return BEnd();
  }

  MOZ_ASSERT(aShapeType == ShapeType::ShapeOutside);
  if (!mShapeInfo) {
    return BEnd();
  }
  // Clip the flow area to the margin-box. See LineLeft().
  return std::min(BEnd(), mShapeInfo->BEnd());
}

bool
nsFloatManager::FloatInfo::IsEmpty(ShapeType aShapeType) const
{
  if (aShapeType == ShapeType::Margin) {
    return IsEmpty();
  }

  MOZ_ASSERT(aShapeType == ShapeType::ShapeOutside);
  if (!mShapeInfo) {
    return IsEmpty();
  }
  return mShapeInfo->IsEmpty();
}

/////////////////////////////////////////////////////////////////////////////
// ShapeInfo

/* static */ LogicalRect
nsFloatManager::ShapeInfo::ComputeShapeBoxRect(
  const StyleShapeSource& aShapeOutside,
  nsIFrame* const aFrame,
  const LogicalRect& aMarginRect,
  WritingMode aWM)
{
  LogicalRect rect = aMarginRect;

  switch (aShapeOutside.GetReferenceBox()) {
    case StyleGeometryBox::ContentBox:
      rect.Deflate(aWM, aFrame->GetLogicalUsedPadding(aWM));
      MOZ_FALLTHROUGH;
    case StyleGeometryBox::PaddingBox:
      rect.Deflate(aWM, aFrame->GetLogicalUsedBorder(aWM));
      MOZ_FALLTHROUGH;
    case StyleGeometryBox::BorderBox:
      rect.Deflate(aWM, aFrame->GetLogicalUsedMargin(aWM));
      break;
    case StyleGeometryBox::MarginBox:
      // Do nothing. rect is already a margin rect.
      break;
    case StyleGeometryBox::NoBox:
    default:
      MOZ_ASSERT(aShapeOutside.GetType() != StyleShapeSourceType::Box,
                 "Box source type must have <shape-box> specified!");
      break;
  }

  return rect;
}

/* static */ UniquePtr<nsFloatManager::ShapeInfo>
nsFloatManager::ShapeInfo::CreateShapeBox(
  nsIFrame* const aFrame,
  const LogicalRect& aShapeBoxRect,
  WritingMode aWM,
  const nsSize& aContainerSize)
{
  nsRect logicalShapeBoxRect
    = ConvertToFloatLogical(aShapeBoxRect, aWM, aContainerSize);

  nscoord physicalRadii[8];
  bool hasRadii = aFrame->GetShapeBoxBorderRadii(physicalRadii);
  if (!hasRadii) {
    return MakeUnique<RoundedBoxShapeInfo>(logicalShapeBoxRect,
                                           UniquePtr<nscoord[]>());
  }

  return MakeUnique<RoundedBoxShapeInfo>(logicalShapeBoxRect,
                                         ConvertToFloatLogical(physicalRadii,
                                                               aWM));
}

/* static */ UniquePtr<nsFloatManager::ShapeInfo>
nsFloatManager::ShapeInfo::CreateBasicShape(
  const UniquePtr<StyleBasicShape>& aBasicShape,
  const LogicalRect& aShapeBoxRect,
  WritingMode aWM,
  const nsSize& aContainerSize)
{
  switch (aBasicShape->GetShapeType()) {
    case StyleBasicShapeType::Polygon:
      return CreatePolygon(aBasicShape, aShapeBoxRect, aWM, aContainerSize);
    case StyleBasicShapeType::Circle:
    case StyleBasicShapeType::Ellipse:
      return CreateCircleOrEllipse(aBasicShape, aShapeBoxRect, aWM,
                                   aContainerSize);
    case StyleBasicShapeType::Inset:
      return CreateInset(aBasicShape, aShapeBoxRect, aWM, aContainerSize);
  }
  return nullptr;
}

/* static */ UniquePtr<nsFloatManager::ShapeInfo>
nsFloatManager::ShapeInfo::CreateInset(
  const UniquePtr<StyleBasicShape>& aBasicShape,
  const LogicalRect& aShapeBoxRect,
  WritingMode aWM,
  const nsSize& aContainerSize)
{
  // Use physical coordinates to compute inset() because the top, right,
  // bottom and left offsets are physical.
  // https://drafts.csswg.org/css-shapes-1/#funcdef-inset
  nsRect physicalShapeBoxRect =
    aShapeBoxRect.GetPhysicalRect(aWM, aContainerSize);
  nsRect insetRect =
    ShapeUtils::ComputeInsetRect(aBasicShape, physicalShapeBoxRect);

  nsRect logicalInsetRect =
    ConvertToFloatLogical(LogicalRect(aWM, insetRect, aContainerSize),
                          aWM, aContainerSize);
  nscoord physicalRadii[8];
  bool hasRadii =
    ShapeUtils::ComputeInsetRadii(aBasicShape, insetRect, physicalShapeBoxRect,
                                  physicalRadii);
  if (!hasRadii) {
    return MakeUnique<RoundedBoxShapeInfo>(logicalInsetRect,
                                           UniquePtr<nscoord[]>());
  }

  return MakeUnique<RoundedBoxShapeInfo>(logicalInsetRect,
                                         ConvertToFloatLogical(physicalRadii,
                                                               aWM));
}

/* static */ UniquePtr<nsFloatManager::ShapeInfo>
nsFloatManager::ShapeInfo::CreateCircleOrEllipse(
  const UniquePtr<StyleBasicShape>& aBasicShape,
  const LogicalRect& aShapeBoxRect,
  WritingMode aWM,
  const nsSize& aContainerSize)
{
  // Use physical coordinates to compute the center of circle() or ellipse()
  // since the <position> keywords such as 'left', 'top', etc. are physical.
  // https://drafts.csswg.org/css-shapes-1/#funcdef-ellipse
  nsRect physicalShapeBoxRect =
    aShapeBoxRect.GetPhysicalRect(aWM, aContainerSize);
  nsPoint physicalCenter =
    ShapeUtils::ComputeCircleOrEllipseCenter(aBasicShape, physicalShapeBoxRect);
  nsPoint logicalCenter =
    ConvertToFloatLogical(physicalCenter, aWM, aContainerSize);

  // Compute the circle or ellipse radii.
  nsSize radii;
  StyleBasicShapeType type = aBasicShape->GetShapeType();
  if (type == StyleBasicShapeType::Circle) {
    nscoord radius = ShapeUtils::ComputeCircleRadius(aBasicShape, physicalCenter,
                                                     physicalShapeBoxRect);
    radii = nsSize(radius, radius);
  } else {
    MOZ_ASSERT(type == StyleBasicShapeType::Ellipse);
    nsSize physicalRadii =
      ShapeUtils::ComputeEllipseRadii(aBasicShape, physicalCenter,
                                      physicalShapeBoxRect);
    LogicalSize logicalRadii(aWM, physicalRadii);
    radii = nsSize(logicalRadii.ISize(aWM), logicalRadii.BSize(aWM));
  }

  return MakeUnique<EllipseShapeInfo>(logicalCenter, radii);
}

/* static */ UniquePtr<nsFloatManager::ShapeInfo>
nsFloatManager::ShapeInfo::CreatePolygon(
  const UniquePtr<StyleBasicShape>& aBasicShape,
  const LogicalRect& aShapeBoxRect,
  WritingMode aWM,
  const nsSize& aContainerSize)
{
  // Use physical coordinates to compute each (xi, yi) vertex because CSS
  // represents them using physical coordinates.
  // https://drafts.csswg.org/css-shapes-1/#funcdef-polygon
  nsRect physicalShapeBoxRect =
    aShapeBoxRect.GetPhysicalRect(aWM, aContainerSize);

  // Get physical vertices.
  nsTArray<nsPoint> vertices =
    ShapeUtils::ComputePolygonVertices(aBasicShape, physicalShapeBoxRect);

  // Convert all the physical vertices to logical.
  for (nsPoint& vertex : vertices) {
    vertex = ConvertToFloatLogical(vertex, aWM, aContainerSize);
  }

  return MakeUnique<PolygonShapeInfo>(Move(vertices));
}

/* static */ UniquePtr<nsFloatManager::ShapeInfo>
nsFloatManager::ShapeInfo::CreateImageShape(
  const UniquePtr<nsStyleImage>& aShapeImage,
  float aShapeImageThreshold,
  nsIFrame* const aFrame,
  WritingMode aWM,
  const nsSize& aContainerSize)
{
  MOZ_ASSERT(aShapeImage ==
             aFrame->StyleDisplay()->mShapeOutside.GetShapeImage(),
             "aFrame should be the frame that we got aShapeImage from");

  nsImageRenderer imageRenderer(aFrame, aShapeImage.get(),
                                nsImageRenderer::FLAG_SYNC_DECODE_IMAGES);

  if (!imageRenderer.PrepareImage()) {
    // The image is not ready yet.
    return nullptr;
  }

  nsRect contentRect = aFrame->GetContentRect();

  // Create a draw target and draw shape image on it.
  nsDeviceContext* dc = aFrame->PresContext()->DeviceContext();
  int32_t appUnitsPerDevPixel = dc->AppUnitsPerDevPixel();
  LayoutDeviceIntSize contentSizeInDevPixels =
    LayoutDeviceIntSize::FromAppUnitsRounded(contentRect.Size(),
                                             appUnitsPerDevPixel);

  // Use empty CSSSizeOrRatio to force set the preferred size as the frame's
  // content box size.
  imageRenderer.SetPreferredSize(CSSSizeOrRatio(), contentRect.Size());

  RefPtr<gfx::DrawTarget> drawTarget =
    gfxPlatform::GetPlatform()->CreateOffscreenCanvasDrawTarget(
      contentSizeInDevPixels.ToUnknownSize(),
      gfx::SurfaceFormat::A8);
  if (!drawTarget) {
    return nullptr;
  }

  RefPtr<gfxContext> context = gfxContext::CreateOrNull(drawTarget);
  MOZ_ASSERT(context); // already checked the target above

  ImgDrawResult result =
    imageRenderer.DrawShapeImage(aFrame->PresContext(), *context);

  if (result != ImgDrawResult::SUCCESS) {
    return nullptr;
  }

  // Retrieve the pixel image buffer to create the image shape info.
  RefPtr<SourceSurface> sourceSurface = drawTarget->Snapshot();
  RefPtr<DataSourceSurface> dataSourceSurface = sourceSurface->GetDataSurface();
  DataSourceSurface::ScopedMap map(dataSourceSurface, DataSourceSurface::READ);

  if (!map.IsMapped()) {
    return nullptr;
  }

  MOZ_ASSERT(sourceSurface->GetSize() == contentSizeInDevPixels.ToUnknownSize(),
             "Who changes the size?");

  uint8_t* alphaPixels = map.GetData();
  int32_t stride = map.GetStride();

  // NOTE: ImageShapeInfo constructor does not keep a persistent copy of
  // alphaPixels; it's only used during the constructor to compute pixel ranges.
  return MakeUnique<ImageShapeInfo>(alphaPixels,
                                    stride,
                                    contentSizeInDevPixels,
                                    appUnitsPerDevPixel,
                                    aShapeImageThreshold,
                                    contentRect,
                                    aWM,
                                    aContainerSize);
}

/* static */ nscoord
nsFloatManager::ShapeInfo::ComputeEllipseLineInterceptDiff(
  const nscoord aShapeBoxBStart, const nscoord aShapeBoxBEnd,
  const nscoord aBStartCornerRadiusL, const nscoord aBStartCornerRadiusB,
  const nscoord aBEndCornerRadiusL, const nscoord aBEndCornerRadiusB,
  const nscoord aBandBStart, const nscoord aBandBEnd)
{
  // An example for the band intersecting with the top right corner of an
  // ellipse with writing-mode horizontal-tb.
  //
  //                             lineIntercept lineDiff
  //                                    |       |
  //  +---------------------------------|-------|-+---- aShapeBoxBStart
  //  |                ##########^      |       | |
  //  |            ##############|####  |       | |
  //  +---------#################|######|-------|-+---- aBandBStart
  //  |       ###################|######|##     | |
  //  |     aBStartCornerRadiusB |######|###    | |
  //  |    ######################|######|#####  | |
  //  +---#######################|<-----------><->^---- aBandBEnd
  //  |  ########################|##############  |
  //  |  ########################|##############  |---- b
  //  | #########################|############### |
  //  | ######################## v<-------------->v
  //  |###################### aBStartCornerRadiusL|
  //  |###########################################|
  //  |###########################################|
  //  |###########################################|
  //  |###########################################|
  //  | ######################################### |
  //  | ######################################### |
  //  |  #######################################  |
  //  |  #######################################  |
  //  |   #####################################   |
  //  |    ###################################    |
  //  |      ###############################      |
  //  |       #############################       |
  //  |         #########################         |
  //  |            ###################            |
  //  |                ###########                |
  //  +-------------------------------------------+----- aShapeBoxBEnd

  NS_ASSERTION(aShapeBoxBStart <= aShapeBoxBEnd, "Bad shape box coordinates!");
  NS_ASSERTION(aBandBStart <= aBandBEnd, "Bad band coordinates!");

  nscoord lineDiff = 0;

  // If the band intersects both the block-start and block-end corners, we
  // don't need to enter either branch because the correct lineDiff is 0.
  if (aBStartCornerRadiusB > 0 &&
      aBandBEnd >= aShapeBoxBStart &&
      aBandBEnd <= aShapeBoxBStart + aBStartCornerRadiusB) {
    // The band intersects only the block-start corner.
    nscoord b = aBStartCornerRadiusB - (aBandBEnd - aShapeBoxBStart);
    nscoord lineIntercept =
      XInterceptAtY(b, aBStartCornerRadiusL, aBStartCornerRadiusB);
    lineDiff = aBStartCornerRadiusL - lineIntercept;
  } else if (aBEndCornerRadiusB > 0 &&
             aBandBStart >= aShapeBoxBEnd - aBEndCornerRadiusB &&
             aBandBStart <= aShapeBoxBEnd) {
    // The band intersects only the block-end corner.
    nscoord b = aBEndCornerRadiusB - (aShapeBoxBEnd - aBandBStart);
    nscoord lineIntercept =
      XInterceptAtY(b, aBEndCornerRadiusL, aBEndCornerRadiusB);
    lineDiff = aBEndCornerRadiusL - lineIntercept;
  }

  return lineDiff;
}

/* static */ nscoord
nsFloatManager::ShapeInfo::XInterceptAtY(const nscoord aY,
                                         const nscoord aRadiusX,
                                         const nscoord aRadiusY)
{
  // Solve for x in the ellipse equation (x/radiusX)^2 + (y/radiusY)^2 = 1.
  MOZ_ASSERT(aRadiusY > 0);
  return aRadiusX * std::sqrt(1 - (aY * aY) / double(aRadiusY * aRadiusY));
}

/* static */ nsPoint
nsFloatManager::ShapeInfo::ConvertToFloatLogical(
  const nsPoint& aPoint,
  WritingMode aWM,
  const nsSize& aContainerSize)
{
  LogicalPoint logicalPoint(aWM, aPoint, aContainerSize);
  return nsPoint(logicalPoint.LineRelative(aWM, aContainerSize),
                 logicalPoint.B(aWM));
}

/* static */ UniquePtr<nscoord[]>
nsFloatManager::ShapeInfo::ConvertToFloatLogical(const nscoord aRadii[8],
                                                 WritingMode aWM)
{
  UniquePtr<nscoord[]> logicalRadii(new nscoord[8]);

  // Get the physical side for line-left and line-right since border radii
  // are on the physical axis.
  Side lineLeftSide =
    aWM.PhysicalSide(aWM.LogicalSideForLineRelativeDir(eLineRelativeDirLeft));
  logicalRadii[eCornerTopLeftX] =
    aRadii[SideToHalfCorner(lineLeftSide, true, false)];
  logicalRadii[eCornerTopLeftY] =
    aRadii[SideToHalfCorner(lineLeftSide, true, true)];
  logicalRadii[eCornerBottomLeftX] =
    aRadii[SideToHalfCorner(lineLeftSide, false, false)];
  logicalRadii[eCornerBottomLeftY] =
    aRadii[SideToHalfCorner(lineLeftSide, false, true)];

  Side lineRightSide =
    aWM.PhysicalSide(aWM.LogicalSideForLineRelativeDir(eLineRelativeDirRight));
  logicalRadii[eCornerTopRightX] =
    aRadii[SideToHalfCorner(lineRightSide, false, false)];
  logicalRadii[eCornerTopRightY] =
    aRadii[SideToHalfCorner(lineRightSide, false, true)];
  logicalRadii[eCornerBottomRightX] =
    aRadii[SideToHalfCorner(lineRightSide, true, false)];
  logicalRadii[eCornerBottomRightY] =
    aRadii[SideToHalfCorner(lineRightSide, true, true)];

  if (aWM.IsLineInverted()) {
    // When IsLineInverted() is true, i.e. aWM is vertical-lr,
    // line-over/line-under are inverted from block-start/block-end. So the
    // relationship reverses between which corner comes first going
    // clockwise, and which corner is block-start versus block-end. We need
    // to swap the values stored in top and bottom corners.
    std::swap(logicalRadii[eCornerTopLeftX], logicalRadii[eCornerBottomLeftX]);
    std::swap(logicalRadii[eCornerTopLeftY], logicalRadii[eCornerBottomLeftY]);
    std::swap(logicalRadii[eCornerTopRightX], logicalRadii[eCornerBottomRightX]);
    std::swap(logicalRadii[eCornerTopRightY], logicalRadii[eCornerBottomRightY]);
  }

  return logicalRadii;
}

//----------------------------------------------------------------------

nsAutoFloatManager::~nsAutoFloatManager()
{
  // Restore the old float manager in the reflow input if necessary.
  if (mNew) {
#ifdef DEBUG
    if (nsBlockFrame::gNoisyFloatManager) {
      printf("restoring old float manager %p\n", mOld);
    }
#endif

    mReflowInput.mFloatManager = mOld;

#ifdef DEBUG
    if (nsBlockFrame::gNoisyFloatManager) {
      if (mOld) {
        mReflowInput.mFrame->ListTag(stdout);
        printf(": float manager %p after reflow\n", mOld);
        mOld->List(stdout);
      }
    }
#endif
  }
}

void
nsAutoFloatManager::CreateFloatManager(nsPresContext *aPresContext)
{
  MOZ_ASSERT(!mNew, "Redundant call to CreateFloatManager!");

  // Create a new float manager and install it in the reflow
  // input. `Remember' the old float manager so we can restore it
  // later.
  mNew = MakeUnique<nsFloatManager>(aPresContext->PresShell(),
                                    mReflowInput.GetWritingMode());

#ifdef DEBUG
  if (nsBlockFrame::gNoisyFloatManager) {
    printf("constructed new float manager %p (replacing %p)\n",
           mNew.get(), mReflowInput.mFloatManager);
  }
#endif

  // Set the float manager in the existing reflow input.
  mOld = mReflowInput.mFloatManager;
  mReflowInput.mFloatManager = mNew.get();
}
