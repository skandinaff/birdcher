Before inventing a fix, diff against Khadas armisp-g12b. Khadas defines G12B/VIM3 behaviour; Linux 6.18 defines the API; Radxa is only a forward-port reference.

The reference is a pinned, read-only submodule:

    external/khadas-common_drivers   khadas/common_drivers
                                     branch khadas-vims-5.15.y, pinned at 3a11a86
                                     ISP source under drivers/armisp-g12b/

Read it, never commit into it, and do not advance the pin without saying why.
It is `ignore = all`, so local dirt there will not show up in `git status` --
which also means an accidental edit will pass unnoticed. `git submodule update
--init external/khadas-common_drivers` restores the pinned revision.

This rule has paid for itself repeatedly. Three DS1 bugs were each settled by
reading the reference rather than reasoning from our own code: the missing
`crop_resolution_changed()` call, `sensor_set_iface()` belonging in
`start_streaming()` (the vendor had already moved it and left the old call
commented out), and the DMA writer stride writes. See
docs/tasks/2026-09-15-ds1-1080p-handover.md.
