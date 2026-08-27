# ============================================================================
# MYDOS - MS-DOS 互換のディスクオペレーティングシステム
#
#   make            イメージを作る            → build/dos.img
#   make run        QEMU で起動する (画面付き)
#   make debug      シリアル出力付きで起動する → ホスト側にログが出る
#   make test       自動テストを走らせる
#   make check      イメージを mtools で検証する
#   make clean      生成物を消す
# ============================================================================

NASM    := nasm
QEMU    := qemu-system-i386
PYTHON  := python3
MCOPY   := mcopy
MDIR    := mdir

BUILD   := build
INCDIR  := kernel/inc

NASMFLAGS := -f bin
KFLAGS    := $(NASMFLAGS) -I $(INCDIR)

# シリアルにも出す版。自動テストはこちらを使う。
DBGFLAGS  := $(KFLAGS) -DSERIAL_CONSOLE

IMG     := $(BUILD)/dos.img
IMG_DBG := $(BUILD)/dos-debug.img

STAGE1  := $(BUILD)/stage1.bin
STAGE2  := $(BUILD)/stage2.bin
KERNEL  := $(BUILD)/io.sys
KERNEL_DBG := $(BUILD)/io-debug.sys
SHELL_COM := $(BUILD)/command.com

# ディスクに入れるテストプログラム
PROGS := $(BUILD)/hello.com $(BUILD)/hello.exe $(BUILD)/dostest.com \
         $(BUILD)/fcbtest.com $(BUILD)/tsrtest.com $(BUILD)/ovltest.com \
         $(BUILD)/dosint.com \
         $(BUILD)/ovl.ovl

KDEPS := kernel/io.asm $(wildcard $(INCDIR)/*.inc)

.PHONY: all run debug test js-test check clean

all: $(IMG)

$(BUILD):
	mkdir -p $(BUILD)

# --- ブートローダー ---------------------------------------------------------
$(STAGE1): boot/stage1.asm | $(BUILD)
	$(NASM) $(NASMFLAGS) $< -o $@

$(STAGE2): boot/stage2.asm | $(BUILD)
	$(NASM) $(NASMFLAGS) $< -o $@

# --- カーネル ---------------------------------------------------------------
$(KERNEL): $(KDEPS) | $(BUILD)
	$(NASM) $(KFLAGS) kernel/io.asm -o $@

$(KERNEL_DBG): $(KDEPS) | $(BUILD)
	$(NASM) $(DBGFLAGS) kernel/io.asm -o $@

# --- シェル -----------------------------------------------------------------
$(SHELL_COM): shell/command.asm | $(BUILD)
	$(NASM) $(NASMFLAGS) $< -o $@

# --- テストプログラム -------------------------------------------------------
$(BUILD)/%.com: tests/%.asm | $(BUILD)
	$(NASM) $(NASMFLAGS) $< -o $@

$(BUILD)/hello.exe: tests/hellox.asm tools/mkexe.py | $(BUILD)
	$(NASM) $(NASMFLAGS) tests/hellox.asm -o $(BUILD)/hellox.bin
	$(PYTHON) tools/mkexe.py $(BUILD)/hellox.bin -o $@ -v

# オーバーレイも MZ 形式。AH=4Bh AL=3 はリロケーションだけを当てる。
$(BUILD)/ovl.ovl: tests/ovlbody.asm tools/mkexe.py | $(BUILD)
	$(NASM) $(NASMFLAGS) tests/ovlbody.asm -o $(BUILD)/ovlbody.bin
	$(PYTHON) tools/mkexe.py $(BUILD)/ovlbody.bin -o $@ -v

# --- イメージの組み立て -----------------------------------------------------
#
# mkimage.py がブートセクタと Stage2 を置いて FAT を初期化する。
# 中身のファイルは mtools の mcopy で入れる。BPB が正しく書けていれば
# ホスト側のツールがそのまま使えるので、専用の道具を書かずに済む。
# $(1) = イメージ / $(2) = 入れるカーネル / $(3) = AUTOEXEC.BAT の元
define make_image
	$(PYTHON) tools/mkimage.py --stage1 $(STAGE1) --stage2 $(STAGE2) -o $(1)
	$(MCOPY) -i $(1) -o $(2) ::IO.SYS
	$(MCOPY) -i $(1) -o $(SHELL_COM) ::COMMAND.COM
	$(MCOPY) -i $(1) -o $(BUILD)/hello.com ::HELLO.COM
	$(MCOPY) -i $(1) -o $(BUILD)/hello.exe ::HELLO.EXE
	$(MCOPY) -i $(1) -o $(BUILD)/dostest.com ::DOSTEST.COM
	$(MCOPY) -i $(1) -o $(BUILD)/fcbtest.com ::FCBTEST.COM
	$(MCOPY) -i $(1) -o $(BUILD)/tsrtest.com ::TSRTEST.COM
	$(MCOPY) -i $(1) -o $(BUILD)/ovltest.com ::OVLTEST.COM
	$(MCOPY) -i $(1) -o $(BUILD)/dosint.com ::DOSINT.COM
	$(MCOPY) -i $(1) -o $(BUILD)/ovl.ovl ::OVL.OVL
	$(MCOPY) -i $(1) -o tests/readme.txt ::README.TXT
	$(MCOPY) -i $(1) -o $(3) ::AUTOEXEC.BAT
endef

IMGDEPS := $(STAGE1) $(STAGE2) $(SHELL_COM) $(PROGS) tests/readme.txt

$(IMG): $(KERNEL) $(IMGDEPS) tests/autoexec.bat
	$(call make_image,$@,$(KERNEL),tests/autoexec.bat)

$(IMG_DBG): $(KERNEL_DBG) $(IMGDEPS) tests/autoexec-test.bat
	$(call make_image,$@,$(KERNEL_DBG),tests/autoexec-test.bat)

# --- 実行 -------------------------------------------------------------------
run: $(IMG)
	$(QEMU) -fda $(IMG) -boot a

debug: $(IMG_DBG)
	$(QEMU) -fda $(IMG_DBG) -boot a -serial stdio

# --- 自動テスト -------------------------------------------------------------
test: $(IMG_DBG)
	$(PYTHON) tools/runtest.py $(IMG_DBG)

# 参照実装 (js/) の回帰テスト。node があるときだけ動く。
js-test:
	node js/test-fat12.js

# --- イメージの検証 (ホスト側のツールで読めるか) ----------------------------
check: $(IMG)
	@echo "--- mdir でルートディレクトリを読む ---"
	$(MDIR) -i $(IMG) ::
	@echo
	@echo "--- fsck.fat で整合性を見る ---"
	-fsck.fat -n $(IMG)

clean:
	rm -rf $(BUILD)
