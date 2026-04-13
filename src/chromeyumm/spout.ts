/** Spout input receiver — Win32 shared-memory frame delivery to the browser. */

import { native, cs } from "./ffi.ts";

export const SpoutReceiver = {
  /**
   * Start a Spout receiver for the named sender.
   * @returns receiverId > 0 on success, 0 on failure (Spout not built or sender not found).
   */
  start(senderName: string): number {
    return native.symbols.startSpoutReceiver(cs(senderName)) as number;
  },

  stop(receiverId: number) {
    native.symbols.stopSpoutReceiver(receiverId);
  },

  /**
   * Returns the Win32 named file-mapping name for this receiver
   * (e.g. "SpoutFrame_1"). The browser reads frames from this mapping.
   */
  getMappingName(receiverId: number): string {
    const result = native.symbols.getSpoutReceiverMappingName(receiverId);
    if (!result) return "";
    return typeof result === "string" ? result : String(result);
  },

  getSeq(receiverId: number): number {
    return native.symbols.getSpoutReceiverSeq(receiverId) as number;
  },
};
