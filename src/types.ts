export interface RgbaColor {
  readonly red: number;
  readonly green: number;
  readonly blue: number;
  readonly alpha?: number;
}

export interface OutlineOptions {
  readonly color: RgbaColor;
  /** Outline radius in pixels. */
  readonly width: number;
}

/** WhatsApp sticker-pack fields written into a WebP EXIF chunk, up to 4 KiB. */
export interface ExifOptions {
  readonly packId: string;
  readonly packName: string;
  readonly emojis?: readonly string[];
}
