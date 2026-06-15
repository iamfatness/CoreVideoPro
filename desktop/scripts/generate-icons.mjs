/**
 * Generates CoreVideo Pro desktop icons (PNG + ICO) for electron-builder.
 * Pure Node — no image deps. Brand colors match src/styles.css (.brand-mark).
 */
import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { deflateSync } from "node:zlib";

const here = dirname(fileURLToPath(import.meta.url));
const buildDir = join(dirname(here), "build");
const SIZE = 1024;

const BG = [10, 15, 22];
const TEAL = [68, 193, 161];
const INK = [7, 17, 14];

function crc32(buffer) {
  let crc = 0xffffffff;
  for (let i = 0; i < buffer.length; i += 1) {
    crc ^= buffer[i];
    for (let j = 0; j < 8; j += 1) {
      crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const length = Buffer.alloc(4);
  length.writeUInt32BE(data.length);
  const typeBuf = Buffer.from(type, "ascii");
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(Buffer.concat([typeBuf, data])));
  return Buffer.concat([length, typeBuf, data, crc]);
}

function writePng(path, rgba, width, height) {
  const stride = width * 4;
  const raw = Buffer.alloc((stride + 1) * height);
  for (let y = 0; y < height; y += 1) {
    const row = y * (stride + 1);
    raw[row] = 0;
    rgba.copy(raw, row + 1, y * stride, (y + 1) * stride);
  }
  const signature = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8;
  ihdr[9] = 6;
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;
  const png = Buffer.concat([
    signature,
    chunk("IHDR", ihdr),
    chunk("IDAT", deflateSync(raw)),
    chunk("IEND", Buffer.alloc(0))
  ]);
  writeFileSync(path, png);
}

function setPixel(rgba, size, x, y, color, alpha = 255) {
  if (x < 0 || y < 0 || x >= size || y >= size) {
    return;
  }
  const i = (y * size + x) * 4;
  rgba[i] = color[0];
  rgba[i + 1] = color[1];
  rgba[i + 2] = color[2];
  rgba[i + 3] = alpha;
}

function fillRect(rgba, size, x0, y0, w, h, color) {
  for (let y = y0; y < y0 + h; y += 1) {
    for (let x = x0; x < x0 + w; x += 1) {
      setPixel(rgba, size, x, y, color);
    }
  }
}

function roundedRect(rgba, size, x0, y0, w, h, radius, color) {
  for (let y = y0; y < y0 + h; y += 1) {
    for (let x = x0; x < x0 + w; x += 1) {
      const dx = Math.min(x - x0, x0 + w - 1 - x);
      const dy = Math.min(y - y0, y0 + h - 1 - y);
      const corner =
        (dx < radius && dy < radius)
          ? Math.hypot(radius - dx, radius - dy)
          : 0;
      if (corner > radius) {
        continue;
      }
      setPixel(rgba, size, x, y, color);
    }
  }
}

function drawGlyphC(rgba, size, ox, oy, scale, color) {
  const s = scale;
  const blocks = [
    [0, 0, 2, 6],
    [0, 0, 4, 2],
    [0, 4, 4, 2],
    [2, 2, 2, 2]
  ];
  for (const [x, y, w, h] of blocks) {
    fillRect(rgba, size, ox + x * s, oy + y * s, w * s, h * s, color);
  }
}

function drawGlyphV(rgba, size, ox, oy, scale, color) {
  const s = scale;
  const blocks = [
    [0, 0, 2, 2],
    [4, 0, 2, 2],
    [1, 2, 2, 2],
    [2, 4, 2, 2]
  ];
  for (const [x, y, w, h] of blocks) {
    fillRect(rgba, size, ox + x * s, oy + y * s, w * s, h * s, color);
  }
}

function renderIcon(size) {
  const rgba = Buffer.alloc(size * size * 4);
  for (let y = 0; y < size; y += 1) {
    for (let x = 0; x < size; x += 1) {
      const t = (x + y) / (size * 2);
      const bg = [
        Math.round(BG[0] + (38 - BG[0]) * t * 0.25),
        Math.round(BG[1] + (81 - BG[1]) * t * 0.25),
        Math.round(BG[2] + (70 - BG[2]) * t * 0.25)
      ];
      setPixel(rgba, size, x, y, bg);
    }
  }

  const mark = Math.round(size * 0.62);
  const mx = Math.round((size - mark) / 2);
  const my = Math.round((size - mark) / 2);
  roundedRect(rgba, size, mx, my, mark, mark, Math.round(size * 0.09), TEAL);

  const scale = Math.max(6, Math.round(size * 0.045));
  const glyphW = 6 * scale;
  const glyphH = 6 * scale;
  const gap = Math.round(scale * 0.8);
  const totalW = glyphW * 2 + gap;
  const gx = Math.round((size - totalW) / 2);
  const gy = Math.round((size - glyphH) / 2);
  drawGlyphC(rgba, size, gx, gy, scale, INK);
  drawGlyphV(rgba, size, gx + glyphW + gap, gy, scale, INK);
  return rgba;
}

function writeIco(path, rgba, size) {
  const andMaskRow = Math.ceil(size / 32) * 4;
  const xorSize = size * size * 4;
  const andSize = andMaskRow * size;
  const image = Buffer.alloc(xorSize + andSize);
  rgba.copy(image, 0);

  const header = Buffer.alloc(6);
  header.writeUInt16LE(0, 0);
  header.writeUInt16LE(1, 2);
  header.writeUInt16LE(1, 4);

  const entry = Buffer.alloc(16);
  entry[0] = size >= 256 ? 0 : size;
  entry[1] = size >= 256 ? 0 : size;
  entry[2] = 0;
  entry[3] = 0;
  entry[4] = 1;
  entry[5] = 32;
  entry.writeUInt32LE(40 + xorSize + andSize, 8);
  entry.writeUInt32LE(22, 12);

  const dib = Buffer.alloc(40);
  dib.writeUInt32LE(40, 0);
  dib.writeInt32LE(size, 4);
  dib.writeInt32LE(size * 2, 8);
  dib.writeUInt16LE(1, 12);
  dib.writeUInt16LE(32, 14);
  dib.writeUInt32LE(0, 16);
  dib.writeUInt32LE(xorSize + andSize, 20);

  writeFileSync(path, Buffer.concat([header, entry, dib, image]));
}

mkdirSync(buildDir, { recursive: true });
const rgba = renderIcon(SIZE);
const pngPath = join(buildDir, "icon.png");
const icoPath = join(buildDir, "icon.ico");
writePng(pngPath, rgba, SIZE, SIZE);
writeIco(icoPath, rgba, SIZE);
console.info(`[icons] wrote ${pngPath}`);
console.info(`[icons] wrote ${icoPath}`);