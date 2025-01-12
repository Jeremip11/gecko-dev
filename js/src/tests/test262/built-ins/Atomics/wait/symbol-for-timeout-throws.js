// |reftest| skip-if(!this.hasOwnProperty('SharedArrayBuffer')||!this.hasOwnProperty('Atomics')) -- SharedArrayBuffer,Atomics is not enabled unconditionally
// Copyright (C) 2018 Amal Hussein.  All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-atomics.wait
description: >
  Throws a TypeError if timeout arg is a Symbol
info: |
  Atomics.wait( typedArray, index, value, timeout )

  4.Let q be ? ToNumber(timeout).
    ...
      Symbol Throw a TypeError exception.
features: [Atomics, SharedArrayBuffer, TypedArray, Symbol]
---*/

function getReport() {
  var r;
  while ((r = $262.agent.getReport()) == null) {
    $262.agent.sleep(100);
  }
  return r;
}

$262.agent.start(
  `
$262.agent.receiveBroadcast(function (sab) {  

  var int32Array = new Int32Array(sab);
  var err;
  
  try {
    Atomics.wait(int32Array, 0, 0, Symbol('foo'));
  } catch(e) {
    err = e.name;
  }
  
  $262.agent.report(err);
  $262.agent.leaving();
})
`);

var sab = new SharedArrayBuffer(4);
var int32Array = new Int32Array(sab);

$262.agent.broadcast(int32Array.buffer);

assert.sameValue(getReport(), 'TypeError');

reportCompare(0, 0);
