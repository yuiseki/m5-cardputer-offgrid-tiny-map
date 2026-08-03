# M5Stack Cardputer Offgrid Tiny Map: The whole planet offline, steered by vim keys

## What I built

The M5Stack Cardputer Offgrid Tiny Map is a map browser that renders vector tiles directly from a `planet.pmtiles` file on its microSD card. The card holds an OpenStreetMap based dataset for the entire planet, from zoom 0 through zoom 14, in a single archive of about 78 GiB split across a 128 GB card. Map rendering requires no network connection: the tiles are decoded and drawn on the device from local storage. You pan and zoom with vim style keys, and the device shows its own position from a GPS receiver.

Because drawing the map needs no radio, the Wi-Fi can be used for something else at the same time. That is not a nice side effect. It is the reason the project exists.

## How it actually started

This began by accident, not by design. I had the Cardputer Meshtastic kit on my desk, and I noticed that its GPS would sometimes get a fix even indoors, with better accuracy than I expected. The stock firmware could not show a map, which felt like a waste of that receiver.

Before changing anything, I backed up the stock firmware so I could always restore it, and then I started tinkering. Getting a raster map onto the little screen worked better than I thought it would.

Indoor GPS was not always reliable, though. To fill the gaps, I added Wi-Fi positioning through WiGLE, the community database of access point locations, so the device could estimate where it was from nearby access points when the GPS could not fix. Using WiGLE to get a position made me want to give back to it as well, and not only indoors: I wanted to scan and log access points as I moved around outdoors.

That wish is where the architecture broke down. Contributing to WiGLE means keeping the radio scanning continuously, while fetching raster map tiles means keeping the same radio busy downloading. The two uses fight over one radio, and outdoors, exactly where I most wanted to contribute, the online map made continuous scanning impractical. I kept having to switch the radio between fetching a tile and scanning, every time I moved.

That collision is the real question of this project: could the ESP32-S3 render vector tiles locally, straight from the card, so the map needs no network at all and the radio stays free to scan? To answer it I threw away the raster tile approach and wrote a fully off grid vector renderer that reads `planet.pmtiles` from the microSD card.

Underneath all of this is a motivation I will admit to. I have a mysterious passion for making maps as small and as self contained as possible. My earlier projects were a Super Tiny Map Pager on a Raspberry Pi Zero 2 WH, an Ultra Tiny Map Pager on an ATOMS3R with a 128 by 128 screen, and a Flying Tiny Map carried by a StampFly drone. Each was online in some way. This one is the first that carries its own world.

## Why raster tiles could not hold the planet

My earlier maps drew ready made 256 by 256 PNG raster tiles from a tile server. That is simple, and with PSRAM it is easy, but it cannot go offline at planet scale for two reasons.

The first is size. Under my own measurements of a real tile server, the smallest raster tile is about 1.8 KB and even empty ocean tiles never fall below that. Because the tile count explodes with zoom, a full raster pyramid up to zoom 18 works out, under those assumptions, to hundreds of terabytes, dominated by the number of tiles rather than by detail. You cannot put that on a card. Even a single country as raster tiles is many gigabytes.

The second is that raster tiles normally arrive over the network, which is exactly what I needed to remove.

Vector tiles fix both. The `planet.pmtiles` archive I use, built from OpenStreetMap data through the OpenMapTiles schema, is about 78 GiB for the whole planet through zoom 14. That fits on a 128 GB card. The price is that vector tiles are not images. They are compressed geometry that the device has to decode and draw itself, and on a Cardputer that price is steep.

## Hardware and the memory constraint

The Cardputer is built around an M5Stamp S3A, which is an ESP32-S3. My build has a 240 by 135 LCD, a full keyboard, a microSD slot, Wi-Fi, and a Cap LoRa-1262 module that also carries an ATGM336H GPS receiver.

One fact shaped everything else: this StampS3 has no PSRAM.

My previous ATOMS3R map had 8 MB of PSRAM, which made raster maps comfortable. The Cardputer has none. Once the firmware, the display sprite, and the Wi-Fi stack are running, the largest single block I can allocate is around 110 KB when the heap is fresh, and much less once it has been used. The display buffer alone is about 64 KB of that.

So the defining rule of the project is that a whole map tile is never held in memory at any point. Everything streams.

The data path looks like this, and the important thing about it is what is missing from it, namely any stage that holds a full tile:

![The streaming data path, where no stage holds a whole map tile in RAM](images/diagram-data-path.png)

## Reading a split PMTiles archive from FAT32

The Cardputer mounts its card as FAT32, which the Arduino SD library handles reliably. FAT32 has a hard rule: no single file may reach 4 GiB. The planet archive is 78 GiB in one file, about nineteen times too large.

The fix does not touch the PMTiles format. I split the archive into 2 GiB pieces named `map.pmtiles.000`, `map.pmtiles.001`, and so on, thirty nine of them, and taught the reader to treat them as one continuous stream. Because 2 GiB is a power of two, a global byte offset maps to a piece and a local offset with a shift and a mask:

```
chunk = offset >> 31
local = offset & 0x7FFFFFFF
```

The only awkward case is a read that straddles a piece boundary, which has to be split. That boundary logic is a pure function with no hardware in it, so I tested it exhaustively with an ordinary compiler on a host before it ran on the device.

PMTiles then finds a tile through directories: a root directory points either to a tile or to a leaf directory. My first small test extract, one Tokyo ward, fit entirely in its root directory, so for a long time my leaf handling never actually ran.

The planet is not like that. Its root directory has 3501 entries, and every one of them points to a leaf. The largest leaf directory is about 38 KB compressed and about 101 KB decompressed, holding over fourteen thousand entries, and I cannot hold 101 KB in memory next to the display buffer. So the directory lookup was rewritten to stream. An entry's offset can be written in closed form,

```
offset[match] = (raw[j] - 1) + sum of length[k] for k in j .. match-1
```

where `j` is the last entry at or before the target that carries an explicit offset. That lets the lookup run as two strictly forward passes over the decompressed directory, holding only a few numbers at a time, with the directory inflated on the fly and never materialised.

To trust this, I ran my C++ reader against the real 78 GiB planet file on a workstation and compared its answers, byte for byte, against the reference PMTiles library in Python. Every tile I checked matched exactly, including ones whose data lives more than 50 GiB into the archive.

## Streaming a tile that does not fit in memory

The worst case I measured is central Tokyo at zoom 14: about 822 KB compressed and about 1.47 MB decompressed. Neither fits in the memory I have, so nothing is ever loaded whole.

The MVT decoder reads a tile through a `ByteSource` abstraction, a few hundred bytes at a time, and hands geometry to the renderer one ring at a time. On a host the `ByteSource` is a block of memory, so the decoder has host tests with no hardware. On the device it is a file on the card. The gzip layer streams the same way, pulling input in small pieces and reproducing output through a ring window that only remembers the most recent bytes for back references.

When I first ran this on the device, the number that mattered was not the speed but the memory. After processing the complete 1.47 MB decompressed tile, the free heap returned to the same value it had before. There was no net heap growth, which confirmed that the full tile was never retained. That was the moment I believed the rest was possible.

## Rendering and caching

Decoding and drawing a tile from scratch is slow, many seconds when the data must be inflated first, and panning cannot feel like that. So each tile is rendered once and the result is cached to the card as a small RGB565 image under `vtcache/`. After that, showing a tile is just reading that image back. The first view of a new area is slow; every later visit is fast.

The cache is rendered at 176 by 176 and scaled up to the screen with bilinear interpolation. An earlier version cached at 128 by 128 with nearest neighbour scaling, and the edges of roads and coastlines were visibly blocky; blending during the upscale removed most of that. Labels are drawn separately at full resolution, so Japanese place names stay sharp. Those labels come from a font built into M5GFX and stored in flash, so there are no glyph files to prepare on the card.

In my testing, the first render of a Tokyo view took about 16 seconds, and every later render of the same view dropped to about 0.3 seconds.

## The Americas disappeared

When I first zoomed all the way out to the whole world, the result was wrong in an oddly specific way. The oceans were blue, Europe, Africa and Asia looked like land, and the Americas were simply gone, painted over in ocean colour.

The cause was a shortcut. To keep the renderer small, it filled each ring of a polygon on its own and ignored holes. That is fine for a building or a small lake. It is wrong for the ocean at zoom 0, where the sea is one polygon with the continents cut out as holes. Filling that polygon while ignoring its holes paints the sea straight over the land, and the Americas happened to be a hole inside a single ocean ring, so they vanished, while other continents survived because of how their geometry was arranged.

Underneath that was a second, separate bug: the sea ring at zoom 0 has about 1422 points, and my geometry buffers had been sized for a city tile, not a world coastline, so the ring was being truncated as well.

The real fix was to stop treating rings independently. The renderer now collects all of a feature's rings and fills them together with an even odd rule, which handles holes and nesting correctly regardless of winding direction. The continents came back.

## When Wi-Fi positioning broke the map

The map has an optional Wi-Fi positioning feature, separate from the continuous scanning. It asks the WiGLE API where the visible access points are, which is useful indoors where GPS cannot fix. After I used it, the map sometimes came back broken, showing only the place labels with nothing drawn underneath.

I asked myself the obvious question: if the memory is fine once the lookup finishes, why does the map still fail? So I measured it. After the TLS request, the largest allocatable block fell from about 82 KB to about 41 KB and did not recover, even though the total free heap barely changed. This was not a leak. It was fragmentation. The TLS session that WiGLE needs takes a large allocation, and freeing it does not put the heap back the way it was, because the ESP32 does not compact memory.

The map broke because one step in drawing a cached tile still allocated a single large buffer, about 62 KB, all at once. Once the heap was fragmented that allocation failed, the geometry could not be drawn, and only the tiny label reads survived. Labels with no map.

Three changes fixed it. Drawing a cached tile no longer allocates a big buffer: it streams two rows of the cached image at a time straight from the card and interpolates between them. The gzip window was reduced from 64 KB to 32 KB, which is all a standard gzip stream needs, halving the largest allocation the renderer makes. And when a genuinely new tile must be generated under a fragmented heap, the renderer splits the work into horizontal strips whose height it chooses from the memory actually available, so it can still produce a full quality tile out of small pieces. After that, positioning and then panning to a new area no longer breaks the map.

The three bugs above share one lesson, and it is the lesson of the whole project: a shortcut that works on a small city extract may still be wrong at planet scale.

## Vim style controls and GNSS

Unlike my tilt controlled ATOMS3R map, the Cardputer has a real keyboard, so the controls are keys, and they follow vim. `h` `j` `k` `l` pan, `zo` and `zc` zoom in and out, and `zz` or `gg` jump to the current location. `zi` toggles the information bar, `r` redraws, and `Enter` captures the screen over serial. Both `z` and `g` are prefixes that wait for the next key, exactly as in vim, so I was careful not to bind either of them to a single action and block the sequences that start with them.

The mapping is not arbitrary. In vim, folds are the one concept about how much detail you see, so zoom lives naturally on `zo` and `zc`, and `zz`, which centres the current line, maps onto centring the map on your position. Anyone who uses vim already knows most of these.

The device boots at zoom 0 over latitude 0, longitude 0, showing the whole world, and you fly in from there. Position comes from the ATGM336H over NMEA, and because the GPS also carries the time, the clock is set with no network, so timestamps are correct off the grid.

## Off the grid

This is the part I care about most, and it is the direct payoff of the WiGLE motivation.

Because the map is drawn entirely from the card, I can turn Wi-Fi off completely and the map still works anywhere on Earth, at every zoom the dataset covers, which is zoom 0 through 14. That leaves the radio free for the things that do need it: holding a GPS fix, and running the continuous Wi-Fi scanning that started this whole project.

The earlier maps in this series were online devices that happened to cache some tiles. This one is an offline device that happens to be able to use the network when it wants to. That inversion is the point.

## How I verified it

Most of this could not be checked by looking at the screen, so I leaned on a few habits. The parts that need no hardware, the chunk boundary maths, the directory reader, the geometry decoder, are plain C++ with host tests that run under an ordinary compiler. Screenshots leave the device as exact pixel data over serial, not as photographs of the LCD, so a render can be compared byte for byte and a colour or missing layer bug the eye would forgive gets caught. And for anything about the planet file itself, I checked my reader against the reference implementation on real data rather than trusting my own reading of the format.

## Limitations

This is an experiment, and it has honest limits. The first visit to a new area is slow, because the tile has to be inflated and rendered before it is cached. The vector style is my own simplified rendering, not a faithful copy of any published map style. The coverage is zoom 0 through 14, so there is no street level building detail beyond what that range carries. Preparing the card means obtaining `planet.pmtiles` and splitting it into 2 GiB pieces yourself. And the data is OpenStreetMap through OpenMapTiles, so it must be used with correct attribution under the Open Database License.

None of these change what the device is. They describe what it is not trying to be.

## Reproducing it

### Hardware

* M5Stack Cardputer (M5Stamp S3A / ESP32-S3, no PSRAM)
* Cap LoRa-1262 module with an ATGM336H GNSS receiver, for position
* A microSD card, formatted FAT32. I use 128 GB for the full planet
* The 3D-ANT antenna for the LoRa cap, if fitted

### Build

The firmware uses PlatformIO with the Arduino framework.

```
board = m5stack-stamps3
framework = arduino
lib_deps = m5stack/M5Cardputer
```

```sh
pio run            # build
pio run -t upload  # flash
```

When several ESP32 boards are connected, device numbers like `/dev/ttyACM0` can change on reconnect, so it is safest to pin the upload port by its stable path under `/dev/serial/by-id/`. It is surprisingly easy to flash the wrong board otherwise.

### SD card layout

The firmware reads the split planet from the card root and creates its own render cache. There are no font files to place; Japanese labels use a font built into the firmware.

```
/
├── map.pmtiles.000
├── map.pmtiles.001
├── ...
├── map.pmtiles.038
└── vtcache/        (created and filled by the firmware on demand)
```

### Preparing planet.pmtiles

You need a planet basemap in PMTiles form, built from OpenStreetMap through the OpenMapTiles schema, covering zoom 0 through 14. It is about 78 GiB. Obtain or build it according to that project's instructions and licensing; this repository does not redistribute the data.

Then split it into 2 GiB pieces for FAT32. This command and its round trip are verified: `split` produces `.000`, `.001`, and so on, and concatenating the pieces reproduces the original byte for byte.

```sh
split -b 2147483648 -d -a 3 planet.pmtiles /path/to/sd/map.pmtiles.
```

To check the result, concatenating the pieces must equal the source:

```sh
cat /path/to/sd/map.pmtiles.* | cmp - planet.pmtiles && echo OK
```

On Windows the `split` and `cat` steps differ and I have not verified them; use a Unix like environment or adapt accordingly. TODO: NEEDS VERIFICATION for a Windows native workflow.

## Attribution and license

* Map data: © OpenStreetMap contributors, available under the Open Database License (ODbL).
* Tile schema: OpenMapTiles.
* Tile archive format: PMTiles.
* Japanese label font: the font built into M5GFX.
* WiGLE: use of the WiGLE API and database is subject to WiGLE's terms; observations and lookups should follow them.
* Project firmware: Apache-2.0, with a NOTICE file in the repository.

If you build on this, keep the OpenStreetMap attribution visible, as the license requires.

## Conclusion

The M5Stack Cardputer Offgrid Tiny Map began from something concrete: I wanted the Cardputer to contribute to WiGLE, and to do that the map had to get off the network. Taking it off the network meant carrying the map data locally, and carrying it at planet scale meant vector tiles, and drawing vector tiles on a device with no PSRAM meant streaming every stage and holding no tile whole.

The result fits in a palm, has no PSRAM, and still holds an OpenStreetMap planet on a card and draws it with the radio switched off. You steer it with vim keys, it knows where it is from GPS, and while it draws a map that needs nothing, its Wi-Fi is free to listen and contribute.

It is not a replacement for the map in your pocket, and it is not trying to be. It is a small answer to a question I kept asking: how much of the world can something this small carry, quietly, by itself.
