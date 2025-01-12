// |reftest| skip -- BigInt is not supported
// Copyright (C) 2016 the V8 project authors. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-%typedarray%.prototype.entries
description: >
  The prototype of the returned iterator is ArrayIteratorPrototype
info: |
  22.2.3.6 %TypedArray%.prototype.entries ( )

  ...
  3. Return CreateArrayIterator(O, "key+value").
includes: [testBigIntTypedArray.js]
features: [BigInt, Symbol.iterator, TypedArray]
---*/

var ArrayIteratorProto = Object.getPrototypeOf([][Symbol.iterator]());

testWithBigIntTypedArrayConstructors(function(TA) {
  var sample = new TA([0n, 42n, 64n]);
  var iter = sample.entries();

  assert.sameValue(Object.getPrototypeOf(iter), ArrayIteratorProto);
});

reportCompare(0, 0);
