# Player mods (RomFS)

[English](#english) | [中文](#chinese)

<a id="english"></a>
## English

The app loads **RomFS mods**, and from a mod manager's merge also the **IPS patches of `main`**
([below](#ips-patches-of-main-exefsbuild-idips)). Code modules in `exefs/` (`subsdk` files) are not loaded: the
runtime runs the rewritten `main`, `sdk` and `subsdk0` only. Loose mod files are
used only when one file lists them: `manifest.tsv`. The one exception is a mod manager's merged output, a
folder with `romfs/` inside and no manifest, which is scanned: see
[TKMM](#a-mod-managers-merged-output-tkmm) and [Android](#android) below.

The launcher points `TOTK_ROMFS_MODS` at

```text
~/Library/Application Support/MacNativeGames/TOTK/mods/<version>
```

where `<version>` is the game version the app was built from: `v0` (1.0.0), `v112` (1.1.2), `v121`
(1.2.1) or `v143` (1.4.3).

### What `manifest.tsv` is

Only the files listed in `manifest.tsv` are served. Each line has four tab-separated fields:

```text
<RomFS path>	<size>	<SHA-256 of the mod file>	<SHA-256 of the original, or ->
```

At startup the loader checks each listed file's size and SHA-256, and checks the original's SHA-256
against the game image. `-` in the last field adds a file the game's RomFS does not have (used by mods
that add assets). Paths are case-sensitive and keep the RomFS spelling.

A folder without `manifest.tsv` means "no mod". Files copied into the folder but not listed are not
used. In detail:

- A **new path** that is not listed is treated as absent: the game cannot see the file.
- A **replacement whose size or hash changed** without regenerating the manifest stops the start with
  `RomFS overlay file missing or wrong size` or `RomFS overlay integrity mismatch`.
- A path that exists in the game but is not listed is read from the game, so the copied file has no
  effect.

### Install a mod

1. Quit the game completely.
2. Copy the mod's RomFS paths **directly** into the version folder. If the mod ships a `romfs/` folder,
   copy its contents, not the folder itself:

   ```bash
   MODDIR="$HOME/Library/Application Support/MacNativeGames/TOTK/mods/v121"
   rsync -a --exclude='.DS_Store' --exclude='manifest.tsv' "/path/to/mod/romfs/" "$MODDIR/"
   ```

3. Generate the manifest with the app you built. Run this from the repository root:

   ```bash
   python3 games/totk/scripts/make_mod_manifest.py "$MODDIR" \
     --app "/path/to/Tears of the Kingdom.app" --version v121
   ```

   The script reads `Contents/Resources/totk/extracted/<version>/romfs` from the app, hashes each file
   and its original, leaves out files that are identical to the original, and writes `manifest.tsv`.
   It prints a line such as `...: 12 files (10 replaced, 2 added)`.

4. Start the game and check the log:

   ```bash
   grep "mods/v121 checked" "$HOME/Library/Logs/MacNativeGames/totk.log" | tail -1
   ```

   It should say `.../mods/v121 checked, N files`; additions also print `M files added to the RomFS`.

### Switching versions

The game reads the installed folder as it is; it does not follow the folder you downloaded or unpacked.
If the new version removes files, set the old version folder aside (or delete it after a backup) before
copying, or the removed files stay active. Generate the manifest again after every change.

### Notes

- Do not edit `manifest.tsv` by hand; regenerate it.
- Do not change files while the game runs. The loader records each file's identity at start and stops
  if a file changes mid-session.
- There is one overlay directory for player mods: to use several mods, merge their files into one
  version folder and generate one manifest. Do not nest mod folders. If two mods change the same path,
  choose one file.
- The build replaces a few RomFS files itself: `UI/LayoutArchive/Common.Product.<n>.Nin_NX_NVN.blarc.zs` (quick-menu
  timing; `<n>` is 100, 110 or 140 depending on the version), `Event/EventFlow/DmF_SY_GameOver.bfevfl.zs`, and with the
  2K output `System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs`. A mod that replaces one of them stops the start with
  "two overlays replace the same file"; leave that file out of the mod for now (UI translation mods hit this, #73).
- Of `exefs/`, only IPS patches of `main` are used, and only from a mods folder with `exefs/` in or beside it (see
  below); `subsdk` code modules are not loaded. (`TOTK_PCHTXT` is a developer experiment for pchtxt patches, not a
  general mod loader.)
- This guide covers the Mac app and, below, Android; installing mods into the iPhone/iPad app container is not covered.

### A mod manager's merged output (TKMM)

[TKMM](https://github.com/TKMM-Team/Tkmm) merges any number of mods against your own dump and writes one folder with
`romfs/` and `exefs/` inside (*Merged* beside the program, or the export folder you set). Merge for the game version the
app runs (1.4.3 on Android), then copy **both folders as they are** into the mods folder:

```text
<mods folder>/romfs/Pack/Actor/...
<mods folder>/romfs/RSDB/...
<mods folder>/exefs/...
```

A mods folder that has **no `manifest.tsv`** but has a `romfs/` folder is scanned at every start, so there is no manifest
to generate and nothing to regenerate after the next merge:

- Every file under `romfs/` replaces the original of the same path, or is added when the game has no such file.
- Nothing is hashed and no original is compared: the merge was made from your own dump. A merge for another game
  version mostly adds files the game never opens (their names carry the version, such as `Tag.Product.143`).
- `desktop.ini`, `Thumbs.db` and dot files are skipped; links are not followed.
- A file this build replaces itself (the list under *Notes*) stops the start, as with a manifest: the log names the
  file and the two ways out (remove it from the mods folder, or turn off the option that needs the build's copy).
  The Android app replaces none of them at its default settings.
- Of `exefs/`, the IPS patches of `main` are applied (next section). `subsdk` code modules (UltraCam, Self-Recall and
  other exlaunch mods) are **not loaded**, and the log lists them. The RomFS half of such a mod is still served.
- Replace the folder's contents while the game is closed. A `manifest.tsv` in the folder, or inside its `romfs/`, takes
  precedence over the scan; one that cannot be read or parsed stops the start instead of falling back to it.

The log line to look for:

```text
[totk] RomFS mods: .../romfs scanned (no manifest.tsv): 2412 files replace originals, 887 added, ...
```

### IPS patches of main (`exefs/<build id>.ips`)

A merge's `exefs/` folder can hold IPS patches (IPS32 or the classic format), each named after the build id of the
module it patches. The `exefs/` folder is looked for inside the mods folder, or beside it when `TOTK_ROMFS_MODS`
names the merge's `romfs/` folder itself. A file named after `main`'s build id is applied at start, whole or not at
all; a file for another module or game version is left alone, and the log says so. The start stops, naming the file,
the address and the reason, when a record

- lies outside `main`'s code, read-only data and data, overlaps another record, or overwrites a pointer the loader
  relocated;
- changes an instruction this port rewrote to run on the device (or one inside a rewritten sequence, which no longer
  runs);
- puts in an instruction the port would have had to rewrite: one that names register `x18`, a system call, or a
  system-register instruction;
- changes a word that one of this port's own patches replaces (that patch reports it).

`TOTK_MOD_IPS=0` in `totk.env` leaves the files unread. pchtxt files are not read (TKMM writes IPS when it merges).
The log line of a file that was applied:

```text
[totk] mod IPS: .../exefs/277178B7DBA1B6D4949A84778D4ABC58B31B34F5.ips applied, 3 records, 12 bytes in main
```

### Android

**In the app:** *Game data → Mods* imports a folder or a zip that has `romfs` inside (TKMM's *Merged* folder, or one
mod as it was downloaded; `romfs` may sit below other folders). The files are copied into the app's private storage,
next to the saves, so they stay when the game data is moved or deleted; a new import replaces the earlier one only
after it was copied completely, and *Remove the imported mods* deletes it (a mods folder you placed by hand, below, is
not touched and takes precedence when it is in the game folder). Everything outside `romfs/` and
`exefs/` is left out, as are desktop leftovers and a `manifest.tsv`. The row then shows the number of files, their size
and how many code modules will not be loaded.

**By hand:** with no setting to change, the game uses the first of these folders that exists:

1. `mods/v143` in the game data folder (the folder that holds `install.json` and `totk.log`);
2. `mods/v143` in the app's private files (where the import above puts them);
3. `mods/v143` in the app's shared folder on internal storage, `Android/data/com.aure.totk/files`.

The third one is for a PC: game data in the app's private internal storage cannot be reached from outside, but the
shared folder can (`adb`), and the app reads it without any permission. Uninstalling the app removes it. `TOTK_ROMFS_MODS=<folder>` in `totk.env` names another folder and
`TOTK_ROMFS_MODS=0` turns mods off. `totk.log` says which folder was chosen (`[totk-app] mods folder ...`).

Both forms work in that folder: a TKMM merge (`mods/v143/romfs`, `mods/v143/exefs`) or loose files with a
`manifest.tsv`. A `manifest.tsv` inside `romfs/` is honoured too (its paths are relative to `romfs/`), so a merge can
be given the hashed check without rearranging it.

```bash
adb push Merged/romfs Merged/exefs /data/local/tmp/tkmm/
adb shell 'D=/sdcard/Android/data/com.aure.totk/files/mods; mkdir -p $D/v143 &&
  cp -r /data/local/tmp/tkmm/romfs /data/local/tmp/tkmm/exefs $D/v143/ &&
  chmod -R o+rX,go-w $D && rm -r /data/local/tmp/tkmm'
```

Two things here are deliberate. `adb push` straight into `Android/data` is refused on devices that mount it directly
(`remote fchown failed: Operation not permitted`, seen on an AYN Thor with Android 13), so the files are pushed to
`/data/local/tmp` and copied on the device. And the `chmod` matters: files copied by `adb` belong to the shell user,
so the app can read them only when they are readable by others.

<a id="chinese"></a>
## 中文

App 加载 **RomFS mod**，对 mod 管理器的合并输出还会应用 **`main` 的 IPS 补丁**
（[见下文](#main-的-ips-补丁exefsbuild-idips)）。`exefs/` 里的代码模块（`subsdk` 文件）不会被加载：运行时只运行
改写后的 `main`、`sdk` 和 `subsdk0`。散装的 mod 文件是否使用，完全由 `manifest.tsv` 决定；唯一的例外是 mod
管理器的合并输出（目录里有 `romfs/` 而没有清单），它会被扫描，见下文的 TKMM 和 Android 两节。

启动器把 `TOTK_ROMFS_MODS` 指向

```text
~/Library/Application Support/MacNativeGames/TOTK/mods/<version>
```

`<version>` 是构建这个 App 时使用的游戏版本：`v0`（1.0.0）、`v112`（1.1.2）、`v121`（1.2.1）、
`v143`（1.4.3）。

### manifest.tsv 是什么

只有列在 `manifest.tsv` 里的文件才会被提供给游戏。每一行是四个用制表符分开的字段：

```text
<RomFS 路径>	<大小>	<mod 文件的 SHA-256>	<原版文件的 SHA-256，或 ->
```

启动时加载器会逐个核对 mod 文件的大小和 SHA-256，并核对原版文件的 SHA-256。最后一个字段是 `-`
表示这是游戏 RomFS 里没有的新文件（用于新增资源）。路径区分大小写，必须和 RomFS 里的写法一致。

没有 `manifest.tsv` 就等于没有 mod。只把文件复制进目录但不写进清单，游戏不会使用：

- 清单里没有的**新路径**会被当成不存在，游戏完全看不到这个文件。
- 覆盖已有文件但大小或哈希变了、又没有重新生成清单，启动会直接报
  `RomFS overlay file missing or wrong size` 或 `RomFS overlay integrity mismatch`。
- 原版有这个路径、但清单里没列，游戏会读取原版文件，复制进去的文件看起来“没有效果”。

### 安装步骤

1. 先完全退出游戏。
2. 把 mod 的 RomFS 路径**直接**放进版本目录。如果 mod 带一层 `romfs/` 文件夹，复制的是它里面的
   内容，不是 `romfs` 这一层：

   ```bash
   MODDIR="$HOME/Library/Application Support/MacNativeGames/TOTK/mods/v121"
   rsync -a --exclude='.DS_Store' --exclude='manifest.tsv' "/path/to/mod/romfs/" "$MODDIR/"
   ```

3. 用你构建好的 App 生成清单。在仓库根目录执行：

   ```bash
   python3 games/totk/scripts/make_mod_manifest.py "$MODDIR" \
     --app "/path/to/Tears of the Kingdom.app" --version v121
   ```

   脚本会读取 App 里的 `Contents/Resources/totk/extracted/<version>/romfs`，计算每个文件和原版
   文件的哈希，跳过与原始完全相同的文件，然后写入 `manifest.tsv`。成功时会打印类似
   `...: 12 files (10 replaced, 2 added)` 的一行。

4. 启动游戏，确认日志：

   ```bash
   grep "mods/v121 checked" "$HOME/Library/Logs/MacNativeGames/totk.log" | tail -1
   ```

   应出现 `.../mods/v121 checked, N files`；有新增文件时还会打印 `M files added to the RomFS`。

### 换版本时

游戏读取的是安装目录本身，不会跟着你下载或解压的那个文件夹走。如果新版本删掉了旧版本的某些
文件，先把旧的版本目录整个移走（或备份后删除）再复制，否则被删掉的文件会继续生效。每次改动后
都要重新生成清单。

### 注意

- 不要手改 `manifest.tsv`，每次重新生成。
- 游戏运行中不要改 mod 文件；加载器会在启动时记录每个文件的身份，运行中变化会停止游戏。
- 玩家 mod 只有一个覆盖目录：多个 mod 就合并到同一个版本目录，最后生成一份清单；不要把 mod
  文件夹嵌套进去。两个 mod 改同一个路径时，只能选一个文件。
- 构建本身也会替换几个 RomFS 文件：`UI/LayoutArchive/Common.Product.<n>.Nin_NX_NVN.blarc.zs`（快捷菜单时机，`<n>`
  按版本是 100、110 或 140）、`Event/EventFlow/DmF_SY_GameOver.bfevfl.zs`，开 2K 输出时还有
  `System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs`。模组如果也替换其中一个，启动时会报“two overlays replace the
  same file”并停止，目前只能把那个文件从模组里去掉（界面翻译模组会碰到，见 #73）。
- `exefs/` 里只有 `main` 的 IPS 补丁会被使用，而且只来自带有 `exefs/` 的 mod 目录（见下文）；`subsdk` 代码模块
  不会被加载。（`TOTK_PCHTXT` 是开发者的 pchtxt 实验入口，不是通用的 mod 加载器。）
- 本文面向 Mac App，以及下文的 Android；iPhone/iPad 的 App 容器内安装未在此说明。

### mod 管理器的合并输出（TKMM）

[TKMM](https://github.com/TKMM-Team/Tkmm) 会把任意数量的 mod 与你自己的游戏转储合并，输出一个包含 `romfs/` 和
`exefs/` 的文件夹（程序旁边的 *Merged*，或你设置的导出目录）。按 App 运行的游戏版本合并（Android 是 1.4.3），
然后把**这两个文件夹原样**复制进 mod 目录：

```text
<mod 目录>/romfs/Pack/Actor/...
<mod 目录>/romfs/RSDB/...
<mod 目录>/exefs/...
```

mod 目录里**没有 `manifest.tsv`** 但有 `romfs/` 文件夹时，每次启动都会扫描它，不需要生成清单，下次合并后也不用
重新生成：

- `romfs/` 下的每个文件替换同路径的原版文件；游戏没有这个路径时作为新增文件。
- 不计算哈希，也不比对原版：合并本来就是基于你自己的转储。为其他游戏版本做的合并，大多只会新增游戏不会
  打开的文件（文件名带版本号，例如 `Tag.Product.143`）。
- 跳过 `desktop.ini`、`Thumbs.db` 和以点开头的文件；不跟随链接。
- 构建本身也替换的文件（见上面的“注意”）会让启动停止，和使用清单时一样：日志会写出文件名和两种解决办法
  （把它从 mod 目录里删掉，或关闭需要构建版本的那个选项）。Android App 在默认设置下不替换其中任何一个。
- `exefs/` 里 `main` 的 IPS 补丁会被应用（见下一节）；`subsdk` 代码模块（UltraCam、Self-Recall 等 exlaunch mod）
  **不会被加载**，日志会列出它们；这类 mod 的 RomFS 部分仍然会被提供。
- 请在游戏关闭时替换目录内容。目录里（或它的 `romfs/` 里）有 `manifest.tsv` 时以清单为准，不做扫描；清单无法
  读取或解析时启动会停止，不会退回到扫描。

日志里应出现：

```text
[totk] RomFS mods: .../romfs scanned (no manifest.tsv): 2412 files replace originals, 887 added, ...
```

### main 的 IPS 补丁（`exefs/<build id>.ips`）

合并输出的 `exefs/` 目录里可以有 IPS 补丁（IPS32 或经典格式），文件名是它所修补模块的 build id。`exefs/` 在
mod 目录里面查找；如果 `TOTK_ROMFS_MODS` 指向的就是合并输出的 `romfs/` 目录，则在它旁边查找。文件名与 `main`
的 build id 相符的补丁会在启动时整体应用，要么全部生效要么全部不生效；属于其他模块或其他游戏版本的文件不会
被使用，日志会说明。遇到下面的记录时启动会停止，并写出文件、地址和原因：

- 落在 `main` 的代码、只读数据和数据之外，与另一条记录重叠，或覆盖了加载器重定位过的指针；
- 修改了本移植为了在设备上运行而改写过的指令（或位于被改写的指令序列内部、已不再执行的指令）；
- 写入了本移植本来必须改写的指令：使用 `x18` 寄存器的指令、系统调用，或系统寄存器指令；
- 修改了本移植自己的补丁要替换的指令字（由那个补丁报告）。

在 `totk.env` 里写 `TOTK_MOD_IPS=0` 可以不读取这些文件。pchtxt 文件不会被读取（TKMM 合并时会输出 IPS）。
补丁被应用时的日志：

```text
[totk] mod IPS: .../exefs/277178B7DBA1B6D4949A84778D4ABC58B31B34F5.ips applied, 3 records, 12 bytes in main
```

### Android

**在 App 里：** *Game data → Mods* 可以导入一个里面有 `romfs` 的文件夹或 zip（TKMM 的 *Merged* 文件夹，或下载下来的
单个 mod；`romfs` 可以在更深一层的文件夹里）。文件会被复制到 App 的私有存储，和存档放在一起，所以移动或删除游戏
数据时它们不受影响；新的导入只有在完整复制之后才会替换之前的 mod，*Remove the imported mods* 会删除它（下面所说
手动放置的 mod 目录不受影响；放在游戏数据目录里的优先于导入的 mod）。
`romfs/` 和 `exefs/` 之外的内容不会被导入，桌面遗留文件和 `manifest.tsv` 也不会。之后这一行会显示文件数量、大小，
以及有多少个代码模块不会被加载。

**手动放置：** 无需任何设置，游戏使用下面第一个存在的目录：

1. 游戏数据目录（包含 `install.json` 和 `totk.log` 的那个目录）里的 `mods/v143`；
2. App 私有文件里的 `mods/v143`（上面的导入就放在这里）；
3. App 在内部存储上的共享目录 `Android/data/com.aure.totk/files` 里的 `mods/v143`。

第三个是给电脑用的：放在 App 私有内部存储里的游戏数据无法从外部访问，但共享目录可以（`adb`），App 读取它也
不需要任何权限。卸载 App 会删除它。在 `totk.env` 里写 `TOTK_ROMFS_MODS=<目录>`
可以指定别的目录，`TOTK_ROMFS_MODS=0` 关闭 mod。`totk.log` 会写出选用的目录（`[totk-app] mods folder ...`）。

这个目录里两种形式都可以：TKMM 合并输出（`mods/v143/romfs`、`mods/v143/exefs`），或带 `manifest.tsv` 的散装
文件。`romfs/` 里面的 `manifest.tsv` 同样有效（路径相对于 `romfs/`），这样不用重新整理合并输出也能使用带哈希
校验的方式。

```bash
adb push Merged/romfs Merged/exefs /data/local/tmp/tkmm/
adb shell 'D=/sdcard/Android/data/com.aure.totk/files/mods; mkdir -p $D/v143 &&
  cp -r /data/local/tmp/tkmm/romfs /data/local/tmp/tkmm/exefs $D/v143/ &&
  chmod -R o+rX,go-w $D && rm -r /data/local/tmp/tkmm'
```

这里有两点是有意为之。在直接挂载 `Android/data` 的设备上，`adb push` 直接推到该目录会被拒绝
（`remote fchown failed: Operation not permitted`，在 Android 13 的 AYN Thor 上见到），所以先推到
`/data/local/tmp`，再在设备上复制。`chmod` 也不能省：`adb` 复制的文件属于 shell 用户，只有对“其他用户”可读时
App 才能读取。
