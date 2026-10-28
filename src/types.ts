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
