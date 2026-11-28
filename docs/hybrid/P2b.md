# P2b core storage

## Mount and device boundaries

The block layer discovers whole-disk and primary MBR extents. It passes a candidate's first and exclusive last sector to the filesystem mount callback. The ext reader checks that its superblock fits, reads it once and validates filesystem geometry against that extent before walking the tree. On success the same superblock supplies the filesystem end and UUID-derived journal identity. This removes two synchronous two-sector reads per successful mount and gives validation and mounting one consistent copy of the metadata.

The VFS still calls the filesystem's in-kernel read routine for lazy files, including FAT files backed by removable devices. The journal attaches after a successful root mount. Its reserved region must lie beyond the ext filesystem and all occupied MBR partitions. ATA PIO and virtio-blk retain their existing flush semantics. No user syscall or disk format changed.

## Repeatable QEMU check

Build Demo for both architectures using the warnings-as-errors commands in [P0](P0.md) at the revision under test. For partitioned images, add `MBR=1` to the same build invocation. Use fresh evidence directories for each layout.

```sh
for arch in x86_64 aarch64; do
    python3 -B tools/release/storage_smoke.py "$arch" "build/demo/$arch-qemu" "storage-$arch"
done
```

The runner uses a private copy of the input disk. On the first boot it writes /tmp/a, crashes and restarts the framebuffer service, forces journal compaction with 3,000 file replacements, checks sync and powers off. On the second boot it checks the generation, replay count and file contents, renames one file, removes another, syncs and terminates QEMU. On the third boot it verifies the renamed file and the absence of both old paths. The interrupted boot follows a completed sync. Power loss during an outstanding device write requires separate fault injection.

## Phase results

Both x86_64 ATA PIO and AArch64 virtio-blk passed this sequence with whole-disk and primary MBR ext4 images. Both whole-disk runs replayed generation 3 with 274 records after shutdown and 276 after the interrupted boot. The stock release smoke suite also passed both whole-disk targets. These runs used a single QEMU vCPU and trusted emulated storage.
