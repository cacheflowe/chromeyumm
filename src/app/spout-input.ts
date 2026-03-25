import { SpoutReceiver } from "chromeyumm";

export class SpoutInput {
  private receiverId = 0;

  constructor(private senderName: string) {}

  start(): boolean {
    this.receiverId = SpoutReceiver.start(this.senderName);
    if (!this.receiverId) {
      console.warn("[chromeyumm] startSpoutReceiver returned 0 — is Spout built in the DLL?");
      return false;
    }
    console.log(`[chromeyumm] Spout receiver started: '${this.senderName}' id=${this.receiverId}`);
    console.log(`[chromeyumm] Shared memory mapping: SpoutFrame_${this.receiverId}`);
    return true;
  }

  get id(): number { return this.receiverId; }

  stop() {
    if (this.receiverId) {
      SpoutReceiver.stop(this.receiverId);
      this.receiverId = 0;
      console.log("[chromeyumm] Spout receiver stopped.");
    }
  }
}
