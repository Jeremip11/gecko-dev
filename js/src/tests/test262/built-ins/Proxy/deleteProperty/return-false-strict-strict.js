'use strict';
// Copyright (C) 2015 the V8 project authors. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.
/*---
es6id: 9.5.10
description: >
    [[Delete]] (P)

    11. If booleanTrapResult is false, return false.
flags: [onlyStrict]
features: [Reflect]
---*/

var p = new Proxy({}, {
  deleteProperty: function() {
    return false;
  }
});

assert.sameValue(Reflect.deleteProperty(p, "attr"), false);

reportCompare(0, 0);
