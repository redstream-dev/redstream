# rdsanalytics

How to read this page, and how to run the examples: [element reference](README.md).

ROI filtering, overcrowding, direction (with the STOPPED state) and line crossing on the tracked objects of each
stream, on the CPU, from nvdsanalytics config files. It attaches DeepStream-shaped frame and object metas, and, with
`osd-mode`, display meta that draws the rules and the counts. DeepStream counterpart: `nvdsanalytics`
([ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvdsanalytics)).
Built when: always. The metas live in the separate library `libgstrds-analytics-1.0`.

## Example

The committed test detector `tests/models/fixeddet.onnx` finds the same six boxes in every frame; the tracker gives
them ids once their probation is over, and the ROI of [rdsanalytics-config](rdsanalytics-config.md)'s example
(`analytics.txt` there) counts them:

```sh
cat > /tmp/det.txt <<EOF
[property]
onnx-file=$PWD/tests/models/fixeddet.onnx
network-type=0
num-detected-classes=2
parse-bbox-func-name=RdsInferParseYolo
EOF

gst-launch-1.0 videotestsrc num-buffers=30 ! video/x-raw,format=NV12,width=640,height=640 ! m.sink_0 \
  rdsstreammux name=m batch-size=1 width=640 height=640 ! rdsinfer config-file-path=/tmp/det.txt \
  ! rdstracker ll-config-file=samples/tracker/config_tracker_IOU.yml ! rdsanalytics config-file=analytics.txt \
  ! rdsvideoconvert ! "video/x-raw(memory:RDS),format=RGBA" ! rdsosd ! fakesink
```

ROI filtering, overcrowding and `objCnt` work on any objects. Direction and line crossing need tracked objects that
move, so this static detector shows none; the repository's tests run them on a street clip
(`tests/models/fetch-tracker-clip.sh`).

Reading a count from C, in a pad probe after `rdsanalytics` (build with `pkg-config --cflags --libs
redstream-analytics-1.0`). Here it prints 0 for frames 0 to 2, while the tracker's probation removes the
detections, then 6:

```c
#include <rds/rds.h>
#include <rds/rds-analytics-meta.h>

static GstPadProbeReturn
on_batch (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, G_GNUC_UNUSED gpointer user_data)
{
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));

  if (bm == NULL)
    return GST_PAD_PROBE_OK;
  rds_acquire_meta_lock (bm);
  for (GList * f = bm->frame_meta_list; f != NULL; f = f->next) {
    RdsFrameMeta *fm = f->data;

    for (GList * u = fm->frame_user_meta_list; u != NULL; u = u->next) {
      RdsUserMeta *um = u->data;
      guint32 n;

      if (um->base_meta.meta_type == RDS_FRAME_META_ANALYTICS && um->user_meta_data != NULL
          && rds_analytics_frame_meta_get_roi_count (um->user_meta_data, "ALL", &n))
        g_print ("source %u frame %d: %u objects in ALL\n", fm->source_id, fm->frame_num, n);
    }
  }
  rds_release_meta_lock (bm);
  return GST_PAD_PROBE_OK;
}

/* analytics: the rdsanalytics element of the pipeline */
static void
watch_counts (GstElement * analytics)
{
  GstPad *src = gst_element_get_static_pad (analytics, "src");

  gst_pad_add_probe (src, GST_PAD_PROBE_TYPE_BUFFER, on_batch, NULL, NULL);
  gst_object_unref (src);
}
```

## Pads

| Pad | Direction | Presence | Caps |
|---|---|---|---|
| `sink` | sink | always | `video/x-raw(memory:RDS)`, any format, 1–2147483647 × 1–2147483647: batches with an `RdsBatchMeta` (from `rdsstreammux`) |
| `src` | src | always | as the sink |

Buffers pass through in place: pixels are never read or written, only metadata. A buffer without a batch meta or a
`memory:RDS` surface is an element error, "rdsanalytics needs batches from rdsstreammux" (STREAM, FORMAT).

## Properties

Names, types, ranges and defaults are those `gst-inspect-1.0` shows for DeepStream 8.0's nvdsanalytics.

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `config-file` | string | `NULL` | a key file | any state | required, also with `enable=false`: without one, or with a file that fails, the start fails with an ERROR naming the property, the file or the key (ours: DeepStream 8.0 fails the state change too, but posts no ERROR). Set while started (DeepStream 8.0 behaviour), the file is loaded and checked at once and applies from the next buffer, with every stream's state reset, the cumulative counts included; a file that fails then posts an ERROR, so an application that stops on ERROR stops there; otherwise the old config, its state and the property's old value stay (ours) |
| `enable` | boolean | `true` | | NULL, READY | DeepStream 8.0 behaviour. `false`: pass-through with no meta at all, as `[property] enable=0` |
| `unique-id` | uint | `17` | 0–4294967295 | any state | written to the `unique_id` of both metas; read per batch |
| `qos` | boolean | `false` | | any state | accepted: GstBaseTransform's; the element never drops buffers |

## Metadata

- **Reads**, under the batch's meta lock: each frame meta's `pad_index` (the stream) and `buf_pts` (the `objCnt`
  window and history eviction), and each object meta's `rect_params` (its point), `class_id` and `object_id`
  (tracked or not). The mux output's size comes from the batch surface.
- **Writes**, per frame, one `RdsUserMeta` on `frame_user_meta_list` with `meta_type` `RDS_FRAME_META_ANALYTICS`
  (`NVDS_USER_FRAME_META_NVDSANALYTICS`, 32), whose `user_meta_data` is an `RdsAnalyticsFrameMeta`: `ocStatus`,
  `objInROIcnt`, `objLCCurrCnt`, `objLCCumCnt`, `unique_id` and `objCnt`, as DeepStream documents them.
  - Per object with something to report, one `RdsUserMeta` on `obj_user_meta_list` with `meta_type`
    `RDS_OBJ_META_ANALYTICS` (`NVDS_USER_OBJ_META_NVDSANALYTICS`, 33) and an `RdsAnalyticsObjInfo`: `roiStatus`,
    `ocStatus`, `lcStatus`, `dirStatus`, `unique_id` and `objStatus`.
  - With `osd-mode` 1 or 2, display metas on `display_meta_list`; with `osd-mode=2`, the objects' `text_params`
    and border (see [Drawing](#drawing)).

The two structs are C++ (`std::string`, `std::vector`, `std::unordered_map`) with DeepStream's field names, declared
in `rds/rds-analytics-meta.h` (in place of DeepStream's `nvds_analytics_meta.h`) and owned by
`libgstrds-analytics-1.0` (pkg-config `redstream-analytics-1.0`).
- C++ reads the fields directly (`m->objInROIcnt["ALL"]`). It must be built with the library's
  `_GLIBCXX_USE_CXX11_ABI` (the default), and include the header at file scope, never inside `extern "C" {}`.
- C sees opaque types and uses the accessors. Those of the frame meta start with `rds_analytics_frame_meta_`:
  `_get_roi_count`, `_get_oc_status`, `_get_lc_count`, `_get_lc_cum_count`, `_get_obj_count` and the `_foreach_*`
  walks. Those of the object info start with `rds_analytics_obj_info_`: `_n_roi_status` / `_get_roi_status` (and the
  oc and lc pairs), `_get_dir_status` and `_get_obj_status`. None lets an exception out: a NULL argument logs a
  critical and returns FALSE, NULL or 0.
- `rds_batch_meta_copy` and `rdsstreamdemux` copy the metas deeply, and releasing the user meta frees them. Strings
  from the getters belong to the meta. The library is always shared: the user metas point to its copy and release
  functions, which must outlive the code that attached them.

## Config file

nvdsanalytics' key file: `[property]` and the feature groups `[roi-filtering-stream-<n>]`,
`[overcrowding-stream-<n>]`, `[line-crossing-stream-<n>]` and `[direction-detection-stream-<n>]`, `<n>` being the
frame meta's `pad_index`. The format and every key are on [rdsanalytics-config](rdsanalytics-config.md).

## Behaviour

### Element

Each row is either **matches nvdsanalytics (DeepStream 8.0)**: DeepStream 8.0 behaviour as observed (with scripted
object meta), or **ours** (where 8.0 settles nothing or is clearly broken).

| Behaviour | Status | Notes |
|---|---|---|
| Streams | matches nvdsanalytics (DeepStream 8.0) | keyed by the frame meta's `pad_index`, not `source_id`; a new `pad_index` gets fresh state. `rdsstreammux` writes the sink pad's number to both, and its stream events carry the same number, so an event's id is the `pad_index` |
| Frames | matches nvdsanalytics (DeepStream 8.0) | every frame meta of a batch is processed in batch order, partial batches and two frames of one stream included; every frame gets a frame meta, also on a stream without groups (empty maps, `objCnt` filled) |
| Object info | matches nvdsanalytics (DeepStream 8.0) | only on objects with something to report (an ROI, overcrowding or line label, a direction or STOPPED) |
| Object box | ours | the analytics use the object's `rect_params`, not `detector_bbox_info` (8.0 undetermined: the observed runs set both boxes equal) |
| Frame size | matches nvdsanalytics (DeepStream 8.0) | the rules are scaled from `config-width` x `config-height` to the batch surface's size, the mux output. Ours: when that size changes, the rules are scaled again and every stream's state resets, the cumulative counts included, with a warning |
| `rds-stream-reset` | ours | clears the stream's histories, STOPPED state, "already counted" flags, objCnt window and untracked warning; its cumulative line-crossing counts stay. DeepStream 8.0 ignores the event |
| `rds-pad-deleted` | ours | frees the stream's state but keeps its cumulative line-crossing counts, so a source added later on that pad starts fresh and continues them. `rds-stream-eos` alone changes nothing (a stream that restarts on its pad comes with a stream reset). A batch already in flight with that pad's frames can arrive after the event and recreate the stream's state, taking back its kept cumulative counts: harmless (ours). DeepStream 8.0 ignores these events |

### Analytics

What the analytics compute, per frame of a stream. Each row is either **matches nvdsanalytics (DeepStream 8.0)**:
DeepStream 8.0 behaviour as observed (with scripted object meta, the docs being silent), or **ours** (where 8.0 settles
nothing or is clearly broken). Terms: a stream's **history** of an object is its list of points; **n** is the object's
observations so far minus 1; **d** is the newest point minus the history's reference.

| Behaviour | Status | Notes |
|---|---|---|
| Rule scaling | matches nvdsanalytics (DeepStream 8.0) | every vertex scaled from `config-width` x `config-height` to the mux output, x and y separately, and truncated to an integer |
| Scaling arithmetic | ours | `v * out / cfg` in 64-bit integers, multiplied first: equal to 8.0's float result while `v * out` is under 2^24; whether 8.0 divides first is not known |
| Object point | matches nvdsanalytics (DeepStream 8.0) | one point per object, its box's bottom centre floored: (floor(left + width/2), floor(top + height)); the rest of the box is ignored. The sums are in float (ours: 8.0's precision is unknown; its observed boxes give the same points either way) |
| Inside test | matches nvdsanalytics (DeepStream 8.0) | the crossing number on the integer point and vertices: an edge toggles when `(yi > Py) != (yj > Py)` and Px is left of the crossing, truncated. Even-odd for self-intersecting polygons, left and top edges in, right and bottom out, whatever the vertex order |
| ROI filtering | matches nvdsanalytics (DeepStream 8.0) | an object of the group's classes gets the label of every ROI its point is in (`roiStatus`, ` ROI:<label>` in `objStatus`) and is counted in `objInROIcnt`; with `inverse-roi=1` the objects outside, and those are counted |
| `objInROIcnt` | matches nvdsanalytics (DeepStream 8.0) | every ROI-filtering label, then every overcrowding label, with 0 from the first frame. A group with `enable=0` adds nothing, not even its zeros |
| `class-id` | matches nvdsanalytics (DeepStream 8.0) | unset, empty, -1 or a list holding -1: every class. Objects of other classes get none of the group's labels, and no object info for it |
| Overcrowding | matches nvdsanalytics (DeepStream 8.0) | `ocStatus` is TRUE when the ROI's count on this frame is at least `object-threshold` and above 0 (`time-threshold` has no effect). The object's `ocStatus` lists the overcrowding ROIs it is in, whatever the status; overcrowding never appears in `objStatus` |
| `objCnt` | matches nvdsanalytics (DeepStream 8.0) | per class, every object whatever the groups and filters: the mode of the per-frame count over the frames whose PTS is within `obj-cnt-win-in-ms` of this one, both ends included. A frame without the class counts 0. Also on streams without groups |
| `objCnt` ties | ours | the count seen most recently. 8.0 has no stable order (it chose the newest in 35 of 48 tied frames) |
| `objCnt` window | ours | frames before a class was first seen count 0 for it; at most 9000 frames; a PTS that goes back or is missing restarts the window |
| History | matches nvdsanalytics (DeepStream 8.0) | on a stream with a direction or line-crossing group, per (pad_index, object id): one point per frame the object is in (frames it misses don't count), x clamped at 0. The window is the last 50 points; the reference is the integer-truncated mean of its oldest 2 points, or of its oldest 4 once it is full |
| One id twice in a frame | ours | each object adds its own point to the shared history |
| History eviction | ours | a history unseen for more than 30 s of PTS is dropped, so an id seen again later starts afresh and can be counted again. Without usable PTS (none, or gone back) the limit is 900 of the stream's frames, and after 7200 frames it goes whatever the PTS (a frozen PTS). Histories are swept once a second of PTS and every 30 frames, so one can outlive its age by up to a sweep. 8.0's limit is unknown (it kept a history across a 30-frame absence, as this does) |
| Direction | matches nvdsanalytics (DeepStream 8.0) | from n = 10, when \|d\| >= 55 px: every direction rule whose vector is within 60° of d (cos >= 0.5), in config order, as ` DIR:<label> ` pieces in `dirStatus` and `objStatus`. A zero vector never matches. `mode` is ignored |
| STOPPED | matches nvdsanalytics (DeepStream 8.0) | only with a direction group, from n = 50: when the window's population standard deviations are σx <= 20 and σy <= 0.1·√h, the object is STOPPED: ` STOPPED ` in `objStatus` and no direction |
| STOPPED limit | ours | h is the current box height (8.0's is undetermined), and 0.1 is the coefficient that best fits 8.0's runs (each measured limit within 0.5 % of it) |
| Direction `class-id` | ours | objects of other classes get no direction or STOPPED, as for the other groups (not observed on 8.0), but still build their history, which line crossing shares |
| Line crossing | matches nvdsanalytics (DeepStream 8.0) | "past" is the side of the line the rule's vector points to. The reference must be strictly before the line, and the newest point on or past it (`loose`) or strictly past it (`balanced`, `strict`). Then d must be within the mode's angle of the vector and long enough: `loose` under 90° and 32 px from n = 3, `balanced` under 60° and 55 px from n = 10, `strict` 78 px from n = 20. Any other `mode` reads as `loose` |
| Strict angle | ours | cos > 0.8 (under 36.87°). 8.0 counted a d 36.24° off the vector and not one at exactly cos = 0.8, so its limit is in [36.24°, 36.87°) (the docs give none; 36° would count two of 8.0's crossings late) |
| `extended` | matches nvdsanalytics (DeepStream 8.0) | 0: the crossing must hit the segment, lengthened by half the box width at each end, strictly; 1: anywhere on the infinite line |
| Segment hit | ours | with the current box's width (8.0's observed boxes kept theirs); on a slanted segment, the point where reference → newest meets the line, projected on it (8.0 was observed only on horizontal segments crossed straight down) |
| Line sides | ours | exact integer cross products: a point exactly on a slanted line is on it, and counts on that frame in `loose` mode (8.0's float slope counts it a frame later). A vector parallel to its line, a zero vector or a line of one point never counts |
| Line-crossing reference | ours | the mean of the oldest 4 points once the window is full, as for direction (8.0's runs can't tell 2 from 4 there) |
| Counting | matches nvdsanalytics (DeepStream 8.0) | an object counts once per line, never against the vector, and never when already past the line at n <= 1. `lcStatus` and ` LC:<label> ` appear on the counting frame only; `objLCCurrCnt` is that frame's count, `objLCCumCnt` the stream's total. No STOPPED gate. Objects of other classes than `class-id`'s are not counted |
| `objStatus` | matches nvdsanalytics (DeepStream 8.0) | ` ROI:<l>` per ROI, ` DIR:<l> ` per direction, ` LC:<l> ` per line counted, then ` STOPPED `; `""` when none |
| Untracked objects | ours | objects without a tracker id count for the ROIs, overcrowding and `objCnt`; direction and line crossing skip them, with one `GST_WARNING` per stream (again after a stream reset). 8.0 turns direction and line crossing off for the stream for good |
| Config reload | matches nvdsanalytics (DeepStream 8.0) | every stream's state resets, the cumulative counts included (see the `config-file` row under [Properties](#properties)) |
| Stream reset and removal | ours | see the `rds-stream-reset` and `rds-pad-deleted` rows under [Element](#element). 8.0 ignores stream events, so a source re-added on a pad inherits the old state |
| Config errors | ours | 8.0 crashes, reads garbage or silently misbehaves on: an empty value, a line crossing without exactly 8 values, an ROI with an odd count, a `config-width` or `config-height` of 0, a label used twice in a group or by both the roi-filtering and overcrowding groups of a stream. All are start errors naming the key (see [rdsanalytics-config](rdsanalytics-config.md#format-and-loading)) |

### Drawing

`rdsanalytics` never draws pixels: it attaches display metas (and, in `osd-mode=2`, writes the objects' text and border)
for `rdsosd` or any OSD downstream. Each row is either **matches nvdsanalytics (DeepStream 8.0)**: DeepStream 8.0
behaviour as observed (in its display-meta dumps), or **ours**.

| Behaviour | Status | Notes |
|---|---|---|
| Elements | matches nvdsanalytics (DeepStream 8.0) | lines and texts only, no rects, arrows or circles: every line `line_width` 2, every text `font_name` "Serif" at `display-font-size` on black (`set_bg_clr` 1, `text_bg_clr` (0,0,0,1)); coordinates truncated to integers |
| `osd-mode=0` | matches nvdsanalytics (DeepStream 8.0) | no display meta, objects untouched (only `display-obj-cnt`'s text) |
| `osd-mode=1` | matches nvdsanalytics (DeepStream 8.0) | ROI-filtering polygons yellow (1,1,0,1), overcrowding polygons orange (1,0.5,0,1), one line per edge with the closing edge last, label `<label>` at the first vertex; direction rules red (1,0,0,1): the vector and two arrowhead lines, label `<label>` at the start point; line-crossing rules green (0,1,0,1): the direction vector with its arrowhead, then the line, label `"<label> "` (a trailing space) at the line's second point. Only enabled groups are drawn. Objects untouched |
| Arrowheads | matches nvdsanalytics (DeepStream 8.0) | two lines ending at the tip, 15 % of the vector's length, at −15° then +15° from the vector (200 px east from (100,650): (271,657) and (271,642) to (300,650)); a vector of zero length gives three lines on its point |
| `osd-mode=2` labels | matches nvdsanalytics (DeepStream 8.0) | as `osd-mode=1`, with `<label>=<objInROIcnt>`, `<label> OverCrowding=<True/False>, Count=<n>` and `<label>=<objLCCumCnt>`; direction labels unchanged |
| `osd-mode=2` objects | matches nvdsanalytics (DeepStream 8.0) | each object with an object info gets `text_params` = its objStatus (an overcrowding-only object: `""`), replacing any text it had, at (left, top − 10), white (1,1,1,1) on black, Serif at `display-font-size`. Its border turns cyan (0,1,1,1) when it has an ROI-filtering or line-crossing label on this frame; a direction, STOPPED or overcrowding alone leave it as it was. Objects without an object info are untouched |
| `display-obj-cnt=1` | matches nvdsanalytics (DeepStream 8.0) | in every `osd-mode`, the first text: `Count for ClassId<c>=<n> ClassId<c>=<n> …` from `objCnt` (sorted by the class id as unsigned, so −1 last) at (5,5), yellow |
| Class −1 in `display-obj-cnt` | ours | printed `ClassId-1` (8.0 prints `ClassId4294967295`). With no class seen yet the text is `Count for` (8.0 not observed) |
| Order | matches nvdsanalytics (DeepStream 8.0) | texts: `display-obj-cnt`'s, then the rules' labels; texts and lines in config order: ROI filtering, overcrowding, direction, line crossing, each group's rules in key order |
| Lines per display meta | matches nvdsanalytics (DeepStream 8.0) | at most 15 lines per display meta, then the next one; every text in the first |
| Texts per display meta | ours | past 16 (`RDS_MAX_ELEMENTS_IN_DISPLAY_META`) texts, the next display meta (8.0 not observed with that many) |
| Display meta list | ours | a display meta is acquired only when something goes in it, and appended to `display_meta_list` in the order they fill. 8.0 adds an extra empty one after a full one and prepends them, the newest first |
| Negative coordinates | ours | 0 (an arrowhead off the frame's top or left, an object whose top is under 10 px); the fields are unsigned (8.0 not observed) |
| Drawn by `rdsosd` | ours | "Serif" is fontconfig's serif (DejaVu Serif in `redstream:dev`), not nvdsosd's font, so the glyphs and text boxes differ. Lines are drawn as `rdsosd` draws any line (the behaviour rows on [its page](rdsosd.md)): width 2 is drawn 1 px wide, as in nvdsosd, and the diagonal arrowheads have hard edges where nvdsosd smooths them slightly |

`rds-stream-reset`, `rds-stream-eos` and `rds-pad-deleted` are `rdsstreammux`'s stream events for one source
([rdsstreammux](rdsstreammux.md), `rds/rds-event.h`).

### Errors

All element errors: at start, no `config-file` ("element property config-file: not set", RESOURCE, NOT_FOUND), a file
that can't be read (RESOURCE, NOT_FOUND) or any other config error (RESOURCE, SETTINGS, naming the file, the group
and the key); while playing, a buffer without batch meta or surface (STREAM, FORMAT), a mux output size the rules
can't be set up for (STREAM, FORMAT), and a new `config-file` that fails (as at start).

## Differences from DeepStream 8.0

- Stream events: `rds-stream-reset` and `rds-pad-deleted` reset or free a stream's state; DeepStream 8.0 ignores them,
  so a source re-added on a pad inherits the old state.
- Untracked objects skip direction and line crossing with one warning per stream; DeepStream 8.0 turns them off for
  the stream for good.
- History eviction after 30 s of PTS (8.0's limit is unknown).
- Config mistakes that crash or misbehave in DeepStream 8.0 are start errors naming the key.
- Every other "ours" row in the tables above.

## Known issues and limits

- On the CPU only; the metas are C++ structs ([Metadata](#metadata) has the ABI rule).
- `time-threshold` (overcrowding) and `mode` (direction detection) are accepted and do nothing, as in DeepStream 8.0.
- Key-file locale suffixes (`roi-A[fr]`) are not read.
