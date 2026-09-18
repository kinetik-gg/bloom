# Data blocks

Data blocks are first-class, typed records in document schema 1.18. They are the durable values
that nodes and sources read; they are not render plans or an implicit event bus. A block carries a
stable ID, kind, type/schema identity, media type, provenance, optional owner and subject, bounded
payload, and tags.

Media `AssetRecord`s remain in the existing `assets` collection and are exposed as media-kind block
adapters. Extension records remain in `extensions` and are exposed as opaque block adapters. This
keeps old storage, asset relinking, and extension round trips intact while giving the Inspector one
typed vocabulary. Authored data blocks are stored in `project.dataBlocks` and use the independent
`dataBlock` allocator namespace.

Typed payloads include sampled scalar curves, colour ramps, typed tables, 2D/3D point sets,
document paths, and mask coverage references. Analysis and opaque blocks carry bounded bytes. The
per-block payload bound is 64 MiB and the project aggregate bound is 128 MiB. Provenance is
persisted so a reader can explain where a value came from without performing media I/O.

AI-task nodes are intentionally one-way. A later AI capability is an asynchronous task node that
produces a data block, reports progress/cancellation/diagnostics through the task system, and never
mutates an existing block or writes directly into the value graph. Readers are ordinary value nodes
and remain deterministic once their input block is selected.

Schema 1.17 projects migrate additively: they gain an empty `dataBlocks` collection and a zero
`dataBlock` allocator high-water value. Existing plan semantics and identity goldens are unchanged;
the new node types only participate when authored in a document.
