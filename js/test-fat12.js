#!/usr/bin/env node
// ============================================================================
// test-fat12.js  -  msdos.js の FAT12 実装を node で検証する
//
// msdos.js はブラウザ向けなので、FAT12Disk のクラス定義だけを切り出し、
// localStorage を差し替えて動かす。
//
// 見ているのは「書いたものがそのまま読み戻せるか」だけ。修正前の実装は
// クラスタを確保した直後に FAT へ印を付けていなかったため、次の
// allocCluster() が同じクラスタを返し、512 バイトを超えるファイルが
// 1 クラスタおきに失われていた。長さも内容も突き合わせるので、
// 同じ欠陥が戻ってくれば必ずここで落ちる。
//
//   使い方: node js/test-fat12.js
// ============================================================================
'use strict';

const fs = require('fs');
const path = require('path');

// --- localStorage の代わり ---------------------------------------------------
const store = new Map();
globalThis.localStorage = {
  getItem: (k) => (store.has(k) ? store.get(k) : null),
  setItem: (k, v) => store.set(k, v),
  removeItem: (k) => store.delete(k),
};

// --- msdos.js から FAT12Disk の定義を取り出す --------------------------------
const src = fs.readFileSync(path.join(__dirname, 'msdos.js'), 'utf8');
const begin = src.indexOf('class FAT12Disk {');
if (begin < 0) throw new Error('msdos.js に FAT12Disk が見つからない');
// download() は Blob と document を使うブラウザ専用のメソッドなので手前で切る
const end = src.indexOf('  download(fn) {', begin);
if (end < 0) throw new Error('FAT12Disk の終わりが見つからない');
const FAT12Disk = eval(`(${src.slice(begin, end)}})`);

// --- テスト -----------------------------------------------------------------
let pass = 0;
let fail = 0;

function check(name, cond, detail) {
  if (cond) {
    pass++;
    console.log(`  [PASS] ${name}`);
  } else {
    fail++;
    console.log(`  [FAIL] ${name}${detail ? '  — ' + detail : ''}`);
  }
}

function pattern(len) {
  const a = new Uint8Array(len);
  for (let i = 0; i < len; i++) a[i] = (i * 7 + 3) & 0xff;
  return a;
}

function roundTrip(disk, name, len) {
  const src = pattern(len);
  disk.writeFile([], name, src);
  const back = disk.readFile([], name);

  if (!back) return `読み戻せなかった`;
  if (back.length !== src.length) {
    return `長さが違う (書いた ${src.length} / 読めた ${back.length})`;
  }
  for (let i = 0; i < src.length; i++) {
    if (src[i] !== back[i]) {
      return `${i} バイト目が違う (期待 ${src[i]} / 実際 ${back[i]})`;
    }
  }

  // クラスタ連鎖の長さも確かめる。内容が合っていても連鎖が短ければ、
  // どこかで同じクラスタを二度使っている。
  const entry = disk
    ._enumEntries(disk._resolveDirSectors([]))
    .find((e) => e.name === name);
  let c = entry.cluster;
  let chain = 0;
  while (c >= 2 && c < 0xff8 && chain < 4096) {
    chain++;
    c = disk.getFAT(c);
  }
  const expect = Math.ceil(len / (512 * disk.SECTORS_PER_CLUSTER)) || 0;
  if (chain !== expect) {
    return `クラスタ連鎖の長さが違う (期待 ${expect} / 実際 ${chain})`;
  }
  return null;
}

console.log('\n=== FAT12Disk: 書き込みと読み戻し ===\n');

const disk = new FAT12Disk();
disk.format();

for (const [name, len] of [
  ['TINY.TXT', 1],
  ['SMALL.TXT', 100],
  ['EXACT.TXT', 512],
  ['OVER.TXT', 513],
  ['BIG.TXT', 1024],
  ['BIG2.TXT', 3000],
  ['HUGE.DAT', 20000],
]) {
  const err = roundTrip(disk, name, len);
  check(`${name.padEnd(10)} ${String(len).padStart(6)} バイト`, err === null, err);
}

console.log('\n=== 削除したクラスタが再利用されるか ===\n');

const beforeFree = disk.freeClusters();
disk.writeFile([], 'TEMP.DAT', pattern(5000));
const afterWrite = disk.freeClusters();
disk.deleteEntry([], 'TEMP.DAT');
const afterDelete = disk.freeClusters();

check('書き込みで空きクラスタが減る', afterWrite < beforeFree,
      `${beforeFree} -> ${afterWrite}`);
check('削除で空きクラスタが元に戻る', afterDelete === beforeFree,
      `${beforeFree} -> ${afterWrite} -> ${afterDelete}`);

console.log(`\n### RESULT pass=${pass} fail=${fail}\n`);
process.exit(fail ? 1 : 0);
