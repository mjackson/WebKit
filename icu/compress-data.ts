#!/usr/bin/env node
// Per-item zstd compression of an ICU common-data package.
//
// Reads an ICU CmnD package (.dat), compresses each item as an individual zstd
// frame using a shared trained dictionary, and emits a libicudata.a containing:
//
//   icudt<NN>_dat          the repacked package (header+TOC unchanged)
//   bun_icu_zstd_dict      the trained dictionary
//   bun_icu_zstd_dict_size u32 dict length
//
// Items matching globs in --skip stay raw so the en-locale cold path is
// zero-cost. The runtime hook lives in Bun (bun_icu_maybe_decompress); ICU's
// udata.cpp calls it via a weak extern (see udata-decompress-hook.patch).
//
// Node stdlib only; shells out to `zstd`. Written for Node's native type
// stripping (erasable annotations only).

import { readFileSync, writeFileSync, mkdtempSync, mkdirSync, rmSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { tmpdir } from "node:os";
import { join } from "node:path";

interface Entry { name: string; nameOff: number; dataOff: number; len: number; out: Uint8Array }

const argv = process.argv.slice(2);
const inPath = argv.shift();
const outA = argv.shift();
let level = 19;
let dictSize = 128 * 1024;
let skipFile = "";
let cc = process.env.CC || "cc";
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === "--level") level = +argv[++i];
  else if (argv[i] === "--dict-size") dictSize = +argv[++i];
  else if (argv[i] === "--skip") skipFile = argv[++i];
  else if (argv[i] === "--cc") cc = argv[++i];
}
if (!inPath || !outA) {
  console.error("usage: node compress-data.ts <in.dat> <out.a> [--level N] [--dict-size N] [--skip file] [--cc CC]");
  process.exit(1);
}

// hot-items.txt globs use only `*` (single path segment) — convert to regex.
const skipRes: RegExp[] = skipFile
  ? readFileSync(skipFile, "utf8")
      .split("\n")
      .map(l => l.replace(/#.*$/, "").trim())
      .filter(Boolean)
      .map(g => new RegExp("^" + g.replace(/[.+^${}()|[\]\\]/g, "\\$&").replace(/\*/g, "[^/]*") + "$"))
  : [];
const isHot = (name: string) => {
  const bare = name.replace(/^icudt\d+[lb]\//, "");
  return skipRes.some(r => r.test(bare));
};

// --- parse package ---
const raw = readFileSync(inPath);
const dv = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
const headerSize = dv.getUint16(0, true);
if (raw[2] !== 0xda || raw[3] !== 0x27) throw new Error("not an ICU data file");
if (String.fromCharCode(raw[12], raw[13], raw[14], raw[15]) !== "CmnD")
  throw new Error("not a CmnD package");

const toc = headerSize;
const count = dv.getUint32(toc, true);
const entries: Entry[] = [];
for (let i = 0; i < count; i++) {
  const o = toc + 4 + i * 8;
  entries.push({
    name: "", nameOff: dv.getUint32(o, true), dataOff: dv.getUint32(o + 4, true), len: 0, out: null as any,
  });
}
for (const e of entries) {
  let p = toc + e.nameOff, end = p;
  while (raw[end] !== 0) end++;
  e.name = raw.subarray(p, end).toString("latin1");
}
const byOff = [...entries].sort((a, b) => a.dataOff - b.dataOff);
for (let i = 0; i < byOff.length; i++) {
  const next = i + 1 < byOff.length ? toc + byOff[i + 1].dataOff : raw.length;
  byOff[i].len = next - (toc + byOff[i].dataOff);
}

// --- train shared dictionary ---
const work = mkdtempSync(join(tmpdir(), "icu-compress-"));
process.on("exit", () => { try { rmSync(work, { recursive: true, force: true }); } catch {} });
const itemsDir = join(work, "items"); mkdirSync(itemsDir);
for (let i = 0; i < byOff.length; i++) {
  const e = byOff[i];
  writeFileSync(join(itemsDir, String(i).padStart(4, "0")), raw.subarray(toc + e.dataOff, toc + e.dataOff + e.len));
}
const dictPath = join(work, "dict.zstdict");
run(["zstd", "-q", "--train", "-r", itemsDir, "-o", dictPath, `--maxdict=${dictSize}`]);
const dict = readFileSync(dictPath);

// --- compress per item (from file, not stdin, so frame header carries content-size) ---
const tmpIn = join(work, "z.in"), tmpOut = join(work, "z.out");
function z(buf: Uint8Array): Uint8Array {
  writeFileSync(tmpIn, buf);
  run(["zstd", "-q", "-f", `-${level}`, "-D", dictPath, tmpIn, "-o", tmpOut]);
  return readFileSync(tmpOut);
}

let kept = 0, comp = 0, rawB = 0, outB = 0;
for (const e of byOff) {
  const src = raw.subarray(toc + e.dataOff, toc + e.dataOff + e.len);
  rawB += src.length;
  if (src.length < 64 || isHot(e.name)) { e.out = src; kept++; outB += src.length; continue; }
  const c = z(src);
  e.out = c.length + 4 >= src.length ? (kept++, src) : (comp++, c);
  outB += e.out.length;
}

// --- rebuild: header verbatim, fresh TOC + name pool + 16-aligned items ---
const tocBytes = 4 + count * 8;
let nameOff = tocBytes;
const nameOffs = new Map<string, number>();
const pool: number[] = [];
for (const e of entries) {
  nameOffs.set(e.name, nameOff);
  for (let i = 0; i < e.name.length; i++) pool.push(e.name.charCodeAt(i));
  pool.push(0);
  nameOff += e.name.length + 1;
}
while ((headerSize + nameOff) % 16) { pool.push(0xaa); nameOff++; }

let dataOff = nameOff;
const dataOffs = new Map<string, number>();
const chunks: Uint8Array[] = [];
for (const e of entries) {
  const pad = (16 - ((headerSize + dataOff) % 16)) % 16;
  if (pad) { chunks.push(new Uint8Array(pad).fill(0xaa)); dataOff += pad; }
  dataOffs.set(e.name, dataOff);
  chunks.push(e.out);
  dataOff += e.out.length;
}

const out = Buffer.allocUnsafe(headerSize + dataOff);
raw.copy(out, 0, 0, headerSize);
out.writeUInt32LE(count, headerSize);
for (let i = 0; i < entries.length; i++) {
  out.writeUInt32LE(nameOffs.get(entries[i].name)!, headerSize + 4 + i * 8);
  out.writeUInt32LE(dataOffs.get(entries[i].name)!, headerSize + 8 + i * 8);
}
Buffer.from(pool).copy(out, headerSize + tocBytes);
let p = headerSize + nameOff;
for (const c of chunks) { Buffer.from(c.buffer, c.byteOffset, c.byteLength).copy(out, p); p += c.length; }

// --- assemble libicudata.a: package + dict + dict-size as .rodata symbols ---
const pkg = entries[0].name.match(/^(icudt\d+)[lb]\//)![1];
const datOut = join(work, "out.dat");
writeFileSync(datOut, out);
writeFileSync(join(work, "dict.bin"), dict);
const asm = join(work, "icudt.S");
writeFileSync(asm, [
  ".section .rodata", ".balign 16",
  `.global ${pkg}_dat`, `.type ${pkg}_dat, @object`, `${pkg}_dat:`,
  `.incbin "${datOut}"`, "",
  ".balign 16", ".global bun_icu_zstd_dict", ".type bun_icu_zstd_dict, @object",
  "bun_icu_zstd_dict:", `.incbin "${join(work, "dict.bin")}"`, ".Ldict_end:", "",
  ".balign 4", ".global bun_icu_zstd_dict_size", ".type bun_icu_zstd_dict_size, @object",
  "bun_icu_zstd_dict_size:", ".long .Ldict_end - bun_icu_zstd_dict", "",
].join("\n"));
const obj = join(work, `${pkg}l_dat.o`);
run([cc, "-c", asm, "-o", obj]);
try { rmSync(outA); } catch {}
run(["ar", "rcs", outA, obj]);

console.error(
  `[icu-compress] ${count} items: ${comp} compressed, ${kept} raw  ` +
  `${rawB}→${outB} (${((100 * outB) / rawB).toFixed(0)}%)  ` +
  `pkg ${raw.length}→${out.length} + dict ${dict.length}`
);

function run(cmd: string[]) {
  const r = spawnSync(cmd[0], cmd.slice(1), { stdio: ["ignore", "ignore", "inherit"] });
  if (r.status !== 0) {
    console.error(`[icu-compress] ${cmd.join(" ")} exited ${r.status}`);
    process.exit(1);
  }
}
