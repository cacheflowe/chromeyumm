/** Screen / display enumeration. */

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
    const result = native.symbols.getAllDisplays();
    if (!result) return [];
    try {
      const str = typeof result === "string" ? result : String(result);
      return JSON.parse(str) as Display[];
    } catch {
      return [];
    }
  },
};
