
export const help = `AVMix is a simple audio video mixer controlled by an updatable JSON playlist format. The filter can be used to:
- schedule video sequence(s) over time
- mix videos together
- layout of multiple videos
- overlay images, text and graphics over source videos

All input streams are decoded prior to entering the mixer.
- audio streams are mixed in software
- video streams are composed according to the \`gpu\` option
- other stream types are not yet supported

OpenGL hardware acceleration can be used, but the supported feature set is currently not the same with our without GPU.

In software mode, the mixer will detect wether any of the currently active video sources can be used as a base canvas for the output to save processing time.
The default behavior is to do this detection only at the first generated frame, use \`dynpfmt\` to modify this.

The filter can be extended through JavaScript modules. Currently only scenes and transition effects use this feature.

# Live vs offline

When operating offline, the mixer will wait for video frames to be ready for 10 times \`lwait\`. After this timeout, the filter will abort if no input is available.
This implies that there shall always be a media to compose, i.e. no "holes" in the timeline.
Note: The playlist is still refreshed in offline mode.


When operating live, the mixer will initially wait for video frames to be ready for \`lwait\` seconds. After this initial timeout, the output frames will indicate:
- 'No signal' if no input is available (no source frames)
- 'Signal lost' if no new input data has been received for \`lwait\` on a source
`;

export const help_playlist = `
# Playlist Format

The playlist describes:
- Media sequences: each sequence is a set of sources to be played continuously
- Transitions: sources in a sequence can be combined using transitions
- Scenes: a scene describes one graphical object to put on screen and if and how input video are mapped on objects
- Timers: a timer can be used to animate scene parameters in various fashions

The playlist content shall be either a single JSON object or an array of JSON objects, hereafter called root objects.
Root objects types can be indicated through a \`type\` property:
- seq: a \`sequence\` object
- url: a \`source\` object (if used as root, a default \`sequence\` object will be created)
- scene: a \`scene\` object
- timer: a \`timer\` object

The \`type\` property of root objects is usually not needed as the parser guesses the object types from its properties.

A root object with a property \`skip\` set to anything but \`0\` or \`false\` is ignored.
Any unrecognized property not starting with \`_\` will be reported as warning.

## JSON syntax

Properties for \`sequence\` objects:
 - id (null): sequence identifier
 - loop (0): number of loops for the sequence (0 means no loop, -1 will loop forever)
 - start (0): sequence start time. If number, offset in seconds from current clock. Otherwise date or \`now\`.
 - stop (0): sequence stop time. If number, offset in seconds from current clock. Otherwise date or \`now\`.
 - transition (null): a \`transition\` object to apply between sources of the sequence
 - seq ([]): array of one or more \`source\ objects

Properties for \`source\` objects:
- id (null): source identifier
- src ([]): list of \`sourceURL\` describing the URLs to play. Multiple sources will be played in parallel
- start (0.0): media start time in source
- stop (0.0): media stop time in source, <=0 means until the end. Ignored if less than equal to \`start\`
- volume (1.0): audio volume (0: silence, 1: input volume), this value is not clamped.
- mix (true): if true, apply sequence transition or mix effect ratio as audio volume. Otherwise volume is not modified by transitions.
- fade ('inout'): indicate how audio should be faded:
  - in: audio fade-in when playing first frame
  - out: audio fade-out when playing last frame
  - inout: both fade-in and fade-out are enabled
  - other: no audio fade
- keep_alive (false): if using dedicated gpac process for one or more input, relaunch process(es) at source end if exit code is greater than 2 or if not responding after \`rtimeout\`
- seek (false): if true and \`keep_alive\` is active, adjust \`start\` according to the time elapsed since source start when relaunching process(es)
- prefetch (500): pre-fetch duration in ms (play before start time of source), 0 for no pre-fetch

Properties for \`sourceURL\` objects:
- port (null): input port for source. Possible values are:
  - pipe: launch a gpac process to play the source using GSF format over pipe
  - tcp, tcpu: launch a gpac process to play the source using GSF format over TCP socket (\`tcp\`) or unix domain TCP socket (\`tcpu\`)
  - not specified or empty string: loads source using the current process
  - other: use value as input filter declaration and launch \'in\' as dedicated process (e.g., in="ffmpeg ..." port="pipe://..."). Any relative URL used in \`in\`' must be relative to the current working directory.
- in (null): filter chain to load as string. Words starting with \`-\` are ignored. The first entry must specifies a source URL, and additional filters and links can be specified using \`@N[#LINKOPT]\` and \`@@N[#LINKOPT]\` syntax, as in gpac
- opts (null): options for the gpac process instance when using dedicated gpac process, ignored otherwise
- media ('all'): filter input media by type, \`a\` for audio, \`v\` for video, \`t\` for text (several characters allowed, e.g. \`av\` or \`va\`), \`all\` accept all input media
- raw (true): indicate if input port is decoded AV (true)) or compressed AV (false) when using dedicated gpac process, ignored otherwise

Note: when launching child process, the input filter is created first and the child process launched afterwards.

Properties for \`scene\` objects:
- id (null): scene identifier
- js ('base'): scene type, either builtin (see below) or path to a JS module
- sources ([]): list of sequence IDs this scene is using. Currently only 0, 1 and 2 values are supported.
- x (0): horizontal coordinate of the scene top-left corner, in percent of the output width (0 means left edge, 100 means right edge)
- x (0): vertical coordinate of the scene top-left corner, in percent of the output height (0 means top edge, 100 means bottom edge)
- width (100): width of the scene, in percent of the output width. Special value \`height\` indicates to use scene height (\`width='height'\` must then be declared after \`height\`)
- height (100): height of the scene, in percent of the output height. Special value \`width\` indicates to use scene width (\`height='width'\` must then be declared after \`width\`)
- zorder (0): display order of the scene
- active (true): indicates if the scene is active or not. An inactive scene will not be rendered nor checked for updates
- rotation (0): rotation angle of the scene in degrees (the rotation is counter-clockwise, around the scene center)
- hskew (0): horizontal skewing factor to apply to the scene
- vskew (0): vertical skewing factor to apply to the scene
- mix (null): a \`transition\` object to apply if more than one source is set, ignored otherwise
- mix_ratio (-1): mix ratio for transition effect, <=0 means first source only, >=1 means second source only

Properties for \`transition\` objects:
- type: transition type, either builtin (see below) or path to a JS module
- dur: transition duration (transitions always end at source stop time)
- fun (null): JS code modifying the ratio effect called \`ratio\`, eg \`fun="ratio = ratio*ratio;"\`

Properties for \`timer\` objects:
- id (null): id of the timer
- dur (0): duration of the timer in seconds
- loop (false): loops timer when \`stop_time\` is not set
- start_time (-1): start time, as offset in seconds from current video time (number) or as date (string) or \`now\`
- stop_time (-1): stop time, as offset in seconds from current video time (number) or as date (string) or \`now\`
- keys ([]): list of keys used for interpolation, ordered list between 0.0 and 1.0
- anims ([]): list of \`animation\` objects

Properties for \`animation\` objects:
- values ([]): list of values to interpolate, there must be as many values as there are keys
- color (false): indicate the values are color (as strings)
- angle (false): indicate the interpolation factor is an angle in degree, to convert to radians (interpolation ratio multiplied by PI and divided by 180) before interpolation
- mode ('linear') : interpolation mode:
  - linear: linear interpolation between the values
  - discrete: do not interpolate
  - other: JS code modifying the interpolation ratio called \`interp\`, eg \`"interp = interp*interp;"\`
- postfun (null): JS code modifying the interpolation result \`res\`, eg \`"res = res*res;"\`
- end ('freeze'): behavior at end of animation:
  - freeze: keep last animated values
  - restore: restore targets to their initial values
- targets ([]): list of strings indicating targets properties to modify. Syntax is:
  - sceneID@option: modifies property \`option\` of given scene
  - sceneID@option[IDX]: modifies value at index \`IDX\ of array property \`option\` of given scene


## Filter configuration
The playlist may specify configuration options of the filter, using a root object of type \'config\':
- property names are the same as the filter options
- property values are given in the native type, or as strings (fractions, vectors, enums)
- each declared property overrides the filter option of the same name (whether default or set at filter creation)

A configuration object in the playlist is only parsed when initially loading the playlist, and ignored when reloading it.


## Playlist modification

The playlist file can be modified at any time.
Objects are identified across playlist reloads through their \`id\` property.
Root objects that are not present after reloading a playlist are removed from the mixer.

A \`sequence\` object modified between two reloads is refreshed, except for its \`start\` field if sequence active.

A \`source\` object shall have the same parent sequence between two reloads. Any modification on the object will only be taken into consideration when (re)loading the source.

A \`sourceURL\` object is not tracked for modification, only evaluated when activating the parent \`source\` object.

A \`scene\` object modified between two reloads is notified of each changed value.

A \`timer\` object modified between two reloads is shut down and restarted. Consequently,  \`animation\` objects are not tracked between updates.

A \`transition\` object may change between two reloads, but any modification on the object will only be taken into consideration when restarting the effect.


## Playlist example

The following is an example playlist using a sequence of two videos with a mix transition and an animated video area:

[
{"id": "seq1", "loop": -1, "start": 0,  "seq":
 [
  { "id": "V1", "src": [{"in": "s1.mp4"}], "start": 60, "stop": 80},
  { "id": "V2", "src": [{"in": "s2.mp4"}], "stop": 100}
 ],
 "transition": { "dur": 1, "type": "mix"}
},
{"id": "scene1", "sources": ["seq1"]},
{"start_time": 0, "dur": 10, "keys": [0, 1], "anims":
 [
  {"values": [50, 0],  "targets": ["scene1@x", "scene1@y"]},
  {"values": [0, 100],  "targets": ["scene1@width", "scene1@height"]}
 ]
 }
]

`;

