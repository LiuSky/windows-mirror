# Daniel Paulus protocol fixtures

These are unmodified test vectors from the MIT-licensed
[`danielpaulus/quicktime_video_hack`](https://github.com/danielpaulus/quicktime_video_hack/tree/main/screencapture/packet/fixtures)
repository. `LICENSE` is the upstream MIT license. They are checked into this
POC so CTest exercises real captured QuickTime/Valeria packets in addition to
small synthetic edge cases.

`asyn-eat` is stored in the exact upstream form, whose four-byte outer packet
length was already removed. The regression test restores that length before
passing it to this implementation's full-packet API. All other selected packet
vectors retain their upstream representation.
