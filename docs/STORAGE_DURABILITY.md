# Root filesystem durability

Chiffon mounts ext read-only and applies changes to the in-memory VFS. The journal occupies the last 4 MiB of the disk, or 128 KiB in the tiny profile. Reuse the same image across boots to retain changes. An image rebuild resets the journal. The ext UUID binds the journal to its base image.

## Commit and recovery

Mounting an ext base requires a clean superblock. Chiffon rejects images requesting ext journal replay, orphan cleanup, or inline-data support. For recovery, preserve the original image and use ext filesystem recovery tools on an offline copy. The Chiffon persistence log is separate from the ext journal.

Partitioned images must use non-overlapping primary MBR partitions. The ext volume must fit inside its selected partition. Chiffon enables persistence only when the entire reserved log area lies beyond all primary partitions, including unused space inside the last partition. GPT and extended partitions currently require a different image layout to boot Chiffon.

Each CHIFLOG2 record begins on a new 512-byte sector. For an append, the kernel clears the next record's first sector, writes the body sectors, and publishes the record's first sector. Each step must complete durably before the next begins. A write touches only sectors outside the committed record prefix. Replay stops at the zero marker or at a torn record that fails validation.

During compaction, the kernel invalidates the inactive slot's header, writes a snapshot of the live changes and deletions, and publishes the higher-generation header last. The active slot remains intact until the new header is durable. A header failure can leave either generation visible on reboot. A retry invalidates the target header before rewriting its snapshot. Boot chooses the newest checksum-valid header matching the ext UUID.

Existing CHIFLOG1 disks replay with their original packed layout. A subsequent persistence operation migrates their live state to the other slot using CHIFLOG2. If the new layout cannot hold the snapshot, migration returns an error and the CHIFLOG1 slot remains available for replay. A kernel that predates CHIFLOG2 can miss changes committed by a newer kernel.

## Errors reported to callers

- `fsync` and `fdatasync` report errors flushing the file and any pending journal repair. The final close of an open-file description reports the same failure and releases the descriptor even on error.
- The kernel's `sync` syscall flushes dirty files and reports a negative errno on failure. Some userspace wrappers cannot expose this return value. Power-off attempts the same flush, prints a warning on failure, and proceeds with shutdown.
- Successful create, delete, rename, and chmod calls report the in-memory VFS result. They attempt to log the change, and a disk failure leaves the change visible in RAM. Failed appends retain dirty state or request a snapshot for the next flush. Follow namespace operations with a checked sync to establish durability.
- If allocation of a deletion tombstone fails during unlink or rename, the namespace change remains visible in RAM and subsequent persistence requests fail with `ENOMEM` for the rest of that boot. This prevents compaction from committing a snapshot that could resurrect the removed path.
- Volatile `/proc` and `/dev` entries and unlinked open files have no disk commit. Initramfs-only boots remain RAM-backed. A disk mount failure falls back to initramfs. If the ext mount succeeds and journal attachment, replay, or the flush probe fails, the boot log identifies the failure, the ext tree remains mounted, and persistence requests return `EIO`. A replay failure can leave changes from the applied prefix visible for that boot.

## Boundaries

The commit guarantees require the backing device and host storage stack to honor flushes and preserve the contents of previously flushed sectors. ATA PIO waits for command completion and issues CACHE FLUSH. The aarch64 virtio block driver negotiates FLUSH and requires it for writes. A read-only device can still replay the existing log. Checksums detect torn records and headers and cannot recover arbitrary media corruption or a failed host that acknowledged flushes prematurely.

Sync may commit multiple records, and each successfully committed record can survive even if a later record fails. A successful compaction may include other pending changes from RAM. A record whose flush reports an error can still survive if the device completed the write. Callers should check recovered state after such an error.

Sector alignment reduces available record capacity for small changes. The snapshot must fit in one slot, including deletion records. Capacity failures return `ENOSPC` and leave the last committed slot available for reboot. The journal and polling drivers depend on the current serialized VFS and block execution model.

## Offline image inspection

Run the inspector with Python 3 on a raw image from a stopped guest. It opens the image read-only.

```sh
python3 tools/inspect-persist.py path/to/disk.img
python3 tools/inspect-persist.py path/to/tiny-disk.img --layout tiny --records
```

The default layout uses the standard 4 MiB reservation. Use `--layout tiny` for the 128 KiB reservation and `--partition 1` through `--partition 4` to match an explicitly selected MBR root partition. `--records` lists validated record types, escaped paths, and payload lengths or rename destinations.

The report shows both headers, their CRC and ext UUID match, the slot the kernel would select, and each checksum-valid record prefix. An invalid header leaves the slot's record area unpublished. After a zero end marker or a validation failure, ignored nonzero bytes can be stale data, unpublished payload, or evidence of a torn write. The bytes alone cannot distinguish these cases. The inspector reads journal metadata without reconstructing the VFS, so a valid prefix can still fail kernel replay. It supports raw ext and primary MBR layouts.

## Human verification

Use the same disk image for each boot and check both aarch64 virtio-blk and x86_64 ATA PIO. Commit an initial file, overwrite it, and stop the guest during the overwrite's close or fsync. Recovery should preserve the initial file or show the complete replacement. Repeat during compaction and legacy migration and check that an interrupted generation switch preserves the committed tree.

Use a file too large for a journal slot to exercise flush failure. A test program should see `ENOSPC` from fsync, the final close, and the raw sync syscall while that file is dirty. Reboot with the original image and verify that previously committed files remain readable. With an existing journal on a read-only backing device, verify replay still works and that a subsequent durability request fails.
