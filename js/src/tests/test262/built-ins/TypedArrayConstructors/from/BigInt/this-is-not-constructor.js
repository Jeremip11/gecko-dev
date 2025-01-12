// |reftest| skip -- BigInt is not supported
// Copyright (C) 2016 the V8 project authors. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.
/*---
esid: sec-%typedarray%.from
description: >
  Throws a TypeError exception if this is not a constructor
info: |
  22.2.2.1 %TypedArray%.from ( source [ , mapfn [ , thisArg ] ] )

  1. Let C be the this value.
  2. If IsConstructor(C) is false, throw a TypeError exception.
  ...
includes: [testBigIntTypedArray.js]
features: [BigInt, TypedArray]
---*/

var m = { m() {} }.m;

testWithBigIntTypedArrayConstructors(function(TA) {
  assert.throws(TypeError, function() {
    TA.from.call(m, []);
  });
});

reportCompare(0, 0);
