# minitouch

Minitouch provides a socket interface for triggering multitouch events and gestures on Android devices. It works without root if started via [ADB](http://developer.android.com/tools/help/adb.html) on SDK 21 or lower. The sole exception is SDK 20 (Android Wear), which does require root. The lowest SDK level we test is 10 (i.e. Android 2.3.3).

It works especially well with HTML5 multitouch events, and unlike the Android [monkey](http://developer.android.com/tools/help/monkey.html) tool, allows you to access the whole screen (including any software buttons).

## Building

Building requires [NDK](https://developer.android.com/tools/sdk/ndk/index.html), and is known to work with at least with NDK Revision 10 (July 2014). *Note that NDK 15 no longer supports anything below Android SDK level 14, meaning that binaries may or may not work on older devices (e.g. Android 2.3).*

We include [libevdev](http://www.freedesktop.org/wiki/Software/libevdev/) as a Git submodule, so first make sure you've fetched it.

```
git submodule init
git submodule update
```

Then it's simply a matter of invoking `ndk-build`.

```
ndk-build
```
You should now have the binaries available in `./libs`.

## Running

You'll need to [build](#building) first. You can then use the included [run.sh](run.sh) script to run the right binary on your device. If you have multiple devices connected, set `ANDROID_SERIAL` before running the script.

To run manually, you have to first figure out which ABI your device supports:

```bash
ABI=$(adb shell getprop ro.product.cpu.abi | tr -d '\r')
```

_Note that as Android shell always ends lines with CRLF, you'll have to remove the CR like above or the rest of the commands will not work properly._

_Also note that if you've got multiple devices connected, setting `$ANDROID_SERIAL` will make things quite a bit easier as you won't have to specify the `-s <serial>` option every time._

Now, push the appropriate binary to the device:

```bash
adb push libs/$ABI/minitouch /data/local/tmp/
```

Note that for SDK <16, you will have to use the `minitouch-nopie` executable which comes without [PIE](http://en.wikipedia.org/wiki/Position-independent_code#Position-independent_executables) support. Check [run.sh](run.sh) for a scripting example.

At this point it might be useful to check the usage:

```bash
adb shell /data/local/tmp/minitouch -h
```

Currently, this should output be something along the lines of:

```
Usage: /data/local/tmp/minitouch [-h] [-d <device>] [-n <name>]
  -d <device>: Use the given touch device. Otherwise autodetect.
  -n <name>:   Change the name of of the abtract unix domain socket. (minitouch)
  -h:          Show help.
````

So, we can simply run the binary without any options, and it will try to detect an appropriate device and start listening on an abstract unix domain socket.

```bash
adb shell /data/local/tmp/minitouch
```

Unless there was an error message and the binary exited, we should now have a socket open on the device. Now we simply need to create a local forward so that we can connect to the socket.

```bash
adb forward tcp:1111 localabstract:minitouch
```

Now you can connect to the socket using the local port. Note that currently **only one connection at a time is supported.** This is mainly because it would otherwise be too easy to submit broken event streams, confusing the driver and possibly freezing the device until a reboot (which, by the way, you'd most likely have to do with `adb reboot` due to the unresponsive screen). Anyway, let's connect.

```bash
nc localhost 1111
```

This will give you some strange output that will be explained in the next section.

## Usage

It is assumed that you now have an open connection to the minitouch socket. If not, follow the [instructions](#running) above.

The minitouch protocol is based on LF-separated lines. Each line is a separate command, and each line begins with a single ASCII letter which specifies the command type. Space-separated command-specific arguments then follow.

When you first open a connection to the socket, you'll get some protocol metadata which you'll need to read from the socket. Other than that there will be no responses of any kind.

### Readable from the socket

#### `v <version>`

Example output: `v 1`

The protocol version. This line is guaranteed to come first in the output. Argument layout may change between versions, so you might want to check if your code supports this version or not.

#### `^ <max-contacts> <max-x> <max-y> <max-pressure>`

Example output: `^ 2 320 480 255`

This gives you the upper bounds of arguments, as reported by the touch device. If you use larger values you will most likely confuse the driver (possibly freezing the screen, requiring a reboot) or the value will simply be ignored.

It's also very important to note that the maximum X and Y coordinates may, but usually do not, match the display size. You'll need to work out a good way to map display coordinates to touch coordinates if required, possibly by using percentages for screen coordinates.

#### `$ <pid>`

Example output: `$ 9876`

This is the pid of the minitouch process. Useful if you want to kill the process.

### Writable to the socket

#### `c`

Example input: `c`

Commits the current set of changed touches, causing the to play out on the screen. Nothing visible will happen until the commit, but depending on the device type, they may already have been buffered.

Commits are not required to list all active contacts. Changes from the previous state are enough.

Same goes for multi-contact touches. The contacts may move around in separate commits or even the same commit. If one contact moves, the others are not required to.

The order of touches in a single commit is not important either. For example you can list contact 5 before contact 0.

Note however that you cannot have more than one `d`, `m` or `u` *for the same `<contact>`* in one commit.

#### `r`

Example input: `r`

Attemps to reset the current set of touches by creating appropriate `u` events and then committing them. As an invalid sequence of events may cause the screen to freeze, you should call for a reset if you have any doubts about the integrity of your events. For example, two `touchstart` events for the same contact is very suspect and most likely means that you lost a `touchend` event somehow.

We try to discard obviously out-of-order events automatically, but sometimes it's not enough.

If the screen freezes you'll have to reboot the device. With careful use this will not happen.

#### `d <contact> <x> <y> <pressure>`

Example input: `d 0 10 10 50`

Schedules a touch down on contact `<contact>` at `<x>,<y>` with `<pressure>` pressure for the next commit.

You cannot have more than one `d`, `m` or `u` *for the same `<contact>`* in one commit.

#### `m <contact> <x> <y> <pressure>`

Example input: `m 0 10 10 50`

Schedules a touch move on contact `<contact>` at `<x>,<y>` with `<pressure>` pressure for the next commit.

You cannot have more than one `d`, `m` or `u` *for the same `<contact>`* in one commit.

#### `u <contact>`

Example input: `u 0`

Schedules a touch up on contact `<contact>`. If you need the contact to move first, use a combination of `m` and `u` separated by a commit.

You cannot have more than one `d`, `m` or `u` *for the same `<contact>`* in one commit.

#### `w <ms>`

Example input: `w 50`

Immediately waits for `<ms>` milliseconds. Will not commit the queue or do anything else.

### Examples

Tap on (10, 10) with 50 pressure using a single contact.

```
d 0 10 10 50
c
u 0
c
```

Long tap on (10, 10) with 50 pressure using a single contact.

```
d 0 10 10 50
c
<wait in your own code>
u 0
c
```

Tap on (10, 10) and (20, 20) simultaneously with 50 pressure using two contacts.

```
d 0 10 10 50
d 1 20 20 50
c
u 0
u 1
c
```

Tap on (10, 10), keep it pressed, then after a while also tap on (20, 20), keep it pressed, then release the first contact and finally release the second contact.

```
d 0 10 10 50
c
<wait in your own code>
d 1 20 20 50
c
<wait in your own code>
u 0
c
<wait in your own code>
u 1
c
```

Swipe from (0, 0) to (100, 0) using a single contact. You'll need to wait between commits in your own code to slow it down.

```
d 0 0 0 50
c
m 0 20 0 50
c
m 0 40 0 50
c
m 0 60 0 50
c
m 0 80 0 50
c
m 0 100 0 50
c
u 0
c
```

Pinch with two contacts going from (0, 100) to (50, 50) and (100, 0) to (50, 50). You'll need to wait between commits in your own code to slow it down.

```
d 0 0 100 50
d 1 100 0 50
c
m 0 10 90 50
m 1 90 10 50
c
m 0 20 80 50
m 1 80 20 50
c
m 0 20 80 50
m 1 80 20 50
c
m 0 30 70 50
m 1 70 30 50
c
m 0 40 60 50
m 1 60 40 50
c
m 0 50 50 50
m 1 50 50 50
c
u 0
u 1
c
```

The same pinch but with more chaotic (or natural) ordering.

```
d 1 100 0 50
c
d 0 0 100 50
c
m 1 90 10 50
m 0 10 90 50
c
m 0 20 80 50
c
m 1 80 20 50
c
m 0 20 80 50
m 1 80 20 50
c
m 0 30 70 50
c
m 1 70 30 50
c
m 1 60 40 50
c
m 0 40 60 50
c
m 0 50 50 50
m 1 50 50 50
c
u 0
c
u 1
c
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

See [LICENSE](LICENSE).

Copyright © CyberAgent, Inc. All Rights Reserved.
