/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { createEnum } = require("devtools/client/shared/enum");

createEnum([

  // Reset font editor to intial state.
  "RESET_EDITOR",

  // Update the value of a variable font axis.
  "UPDATE_AXIS_VALUE",

  // Update font editor with applicable fonts and user-defined CSS font properties.
  "UPDATE_EDITOR_STATE",

  // Toggle the visibiltiy of the font editor
  "UPDATE_EDITOR_VISIBILITY",

  // Update the list of fonts.
  "UPDATE_FONTS",

  // Update the preview text.
  "UPDATE_PREVIEW_TEXT",

], module.exports);
