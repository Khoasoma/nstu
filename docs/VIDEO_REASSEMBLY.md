# Video Frame Reassembly

`FrameReassembler` accepts only packets whose stream and authentication have
already been established by the control plane.

Default bounds:

- 8 incomplete frames;
- 4096 fragments per frame;
- 1400 bytes per fragment;
- 4 MiB per encoded frame;
- 8 MiB total buffered payload;
- 150 ms deadline from the first fragment.

The deadline is never extended by later fragments. This prevents a sender from
retaining receiver memory indefinitely by sending one fragment at a time.

Duplicate fragments with identical content are counted and ignored. A duplicate
index with different content is a conflict and is rejected. Metadata such as
fragment count and capture timestamp must remain identical for every fragment
of a frame. Only the final fragment may carry `end_of_frame`, and the final
fragment must carry it.

Expired incomplete frames are frame-loss events. They are distinct from packet
loss: one missing packet may drop one frame, while several missing packets may
belong to the same frame. `missing_fragments(frame_id)` provides bounded input
for a future NACK policy.

The implementation remembers recently completed frame IDs so late duplicates
cannot allocate a new partial frame. Reassembly limits are allocation guards,
not tuning suggestions; production configuration must remain bounded.
