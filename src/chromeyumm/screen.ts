/** Screen / display enumeration. */

import { CString, type Pointer } from "bun:ffi";
import { native } from "./ffi.ts";

export interface Rectangle {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface Display {
  id: number;
  bounds: Rectangle;
  workArea: Rectangle;
  scaleFactor: number;
  isPrimary: boolean;
}

export const Screen = {
  getAllDisplays(): Display[] {
    const ptr = native.symbols.getAllDisplays() as unknown as Pointer;
    if (!ptr) return [];
    try {
      return JSON.parse(new CString(ptr).toString()) as Display[];
    } catch {
      return [];
    }
  },
};
