# rdsanalytics config file

The config file `rdsanalytics` reads (`config-file`): nvdsanalytics' key file, with every key's groups, type,
default, range and status. The element is on its own page, [rdsanalytics](rdsanalytics.md).

## Example

An ROI over the whole of a 640×640 frame for stream 0, counting every class, with the counts drawn (`osd-mode=2`),
saved as `analytics.txt`:

```ini
[property]
enable=1
config-width=640
config-height=640
osd-mode=2
display-obj-cnt=1

[roi-filtering-stream-0]
enable=1
roi-ALL=0;0;700;0;700;700;0;700
class-id=-1
```

The ROI reaches past the frame on purpose: an object's point is its box's bottom centre, and a point on the bottom
edge (y 640) is outside an ROI whose bottom edge is at 640. [rdsanalytics](rdsanalytics.md) runs it.

## Format and loading

- The `config-file` is a GLib key file. `[property]` is required, with `enable`, `config-width` and `config-height`.
  The feature groups are `[roi-filtering-stream-<n>]`, `[overcrowding-stream-<n>]`, `[line-crossing-stream-<n>]`
  and `[direction-detection-stream-<n>]`, where `<n>` is the frame meta's `pad_index` (DeepStream 8.0 behaviour).
- The docs also spell three groups without `stream` (`[overcrowding-<n>]`, ...): such a group, and
  `[roi-filtering-<n>]`, is skipped with a log line naming it (DeepStream 8.0 behaviour). Any other unknown group is
  skipped with a log line too (ours).
- Unknown keys are ignored silently in `[property]` and with a log line in feature groups (DeepStream 8.0
  behaviour). A key of another feature (`time-threshold` outside overcrowding, `roi-<label>` in a line-crossing
  group) is an unknown key.
- Rules (`roi-<label>`, `line-crossing-<label>`, `direction-<label>`) keep their order in the file. A feature group
  without a rule is an error (DeepStream 8.0 behaviour). Lists are `;`-separated; a trailing `;` is accepted.
- Ranges are written as in the parser's key table (`0..2`, `>= 1`), not as the element pages' `A–B`.
- Defaults are DeepStream 8.0 behaviour (observed: the docs give none) unless marked "(ours)". **supported**: used;
  **accepted no-op**: read, type-checked and ignored.
- Errors name the file, the group and the key: `FILE:[GROUP] KEY: VALUE: reason`. Besides wrong types and ranges,
  these are errors (ours, where 8.0 crashes, reads garbage or silently misbehaves): an empty value (except
  `class-id`), a `config-width` or `config-height` of 0, an ROI with an odd count or fewer than 3 points, a line
  crossing without exactly 8 values, a direction without exactly 4, negative or non-integer coordinates (fatal in
  8.0 too), an empty list item (`1;;2`), a label used twice in one group or in both the
  roi-filtering and the overcrowding group of a stream, an empty label, and a stream number that is
  not one: not a decimal number without leading zeros (`-stream-00`, `-stream-007`), or above 4294967295. So two
  spellings never name one stream. `class-id=` and `class-id=;` mean every class; `mode=` in a direction group is
  accepted.
- Key-file locale suffixes (`roi-A[fr]`) are not read: such a key is an unknown key (ours).

The file is read, and every key checked, at start (READY to PAUSED); an error fails the state change. Setting
`config-file` while playing loads and checks the new file at once; it applies from the next buffer (see the
`config-file` row on [rdsanalytics](rdsanalytics.md#properties)).

## rdsanalytics config keys

| Key | Groups | Type | Default | Range | Status | Notes |
|---|---|---|---|---|---|---|
| `enable` | property | bool | required |  | supported | 0: pass-through, no meta (DeepStream 8.0 behaviour) |
| `config-width` | property | uint | required | >= 1 | supported | the frame size the rule coordinates are in; they are scaled to the mux output. 0 is an error (ours): 8.0 accepts it and every scaled coordinate becomes garbage |
| `config-height` | property | uint | required | >= 1 | supported | as config-width |
| `osd-mode` | property | uint | `2` | 0..2 | supported | 0: no overlay, 1: the rules and their labels, 2: also the counts and statuses |
| `display-font-size` | property | uint | `12` | >= 0 | supported | 0 reads as 12 (DeepStream 8.0 behaviour); no upper bound (ours) |
| `obj-cnt-win-in-ms` | property | uint | `50` (ours) | 1..1000000000 | supported | undocumented: the PTS window of objCnt (the per-class count's mode over the window); the default is ours, within the 33-100 ms measured on DeepStream 8.0 |
| `display-obj-cnt` | property | bool | `0` |  | supported | undocumented: 1 draws the per-class object counts, in every osd-mode (DeepStream 8.0 behaviour) |
| `enable` | roi-filtering, overcrowding, line-crossing, direction-detection | bool | `1` (ours) |  | supported | 0: the feature is off for the stream, and its labels are absent from the metas (DeepStream 8.0 behaviour); 8.0's behaviour without the key is unknown |
| `class-id` | roi-filtering, overcrowding, line-crossing, direction-detection | int list | none | >= -1 | supported | the classes the group's rules apply to; unset, empty or a list holding -1: every class (DeepStream 8.0 behaviour) |
| `inverse-roi` | roi-filtering | bool | `0` |  | supported | 1: the labels go on the objects outside each ROI, and those are counted (DeepStream 8.0 behaviour) |
| `object-threshold` | overcrowding | uint | `0` | >= 0 | supported | overcrowded when the ROI's count is at least this and above 0 (DeepStream 8.0 behaviour); unset flags from a count of 1, as in 8.0 |
| `time-threshold` | overcrowding | uint | `0` | >= 0 | accepted no-op | undocumented; accepted with no effect, as in DeepStream 8.0. In the other feature groups it is an unknown key |
| `mode` | line-crossing | string | `loose` |  | supported | strict, balanced or loose; any other value reads as loose (DeepStream 8.0 behaviour). Matching is case-sensitive (ours: `Strict` reads as loose; 8.0 not observed) |
| `mode` | direction-detection | string | none |  | accepted no-op | accepted and ignored, as in DeepStream 8.0 |
| `extended` | line-crossing | bool | `0` |  | supported | 0: the crossing must hit the segment, 1: the infinite line (DeepStream 8.0 behaviour) |
| `roi-<label>` | roi-filtering, overcrowding | polygon | none | >= 0 | supported | x1;y1;x2;y2;... in order around the polygon, at least 3 points; integers >= 0. A label used by both the roi-filtering and the overcrowding group of a stream is an error (ours): both go in `objInROIcnt`, where 8.0 lets one count overwrite the other. Direction and line-crossing labels are independent. Fewer than 3 points or an odd count is an error (ours): 8.0 accepts them (an ROI that never matches, a missing y read as 0) |
| `line-crossing-<label>` | line-crossing | line | none | >= 0 | supported | 8 integers >= 0: the direction vector (x1;y1;x2;y2), then the line's two ends. Another count is an error (ours): 8.0 reads a short line as garbage and ignores extra values |
| `direction-<label>` | direction-detection | vector | none | >= 0 | supported | 4 integers >= 0: the vector from (x1;y1) to (x2;y2). Another count is an error (ours): 8.0 reads a missing value as 0 |
