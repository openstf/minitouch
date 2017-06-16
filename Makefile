.PHONY: default clean prebuilt

NDKBUILT := \
  libs/arm64-v8a/minitouch \
  libs/arm64-v8a/minitouch-nopie \
  libs/armeabi/minitouch \
  libs/armeabi/minitouch-nopie \
  libs/armeabi-v7a/minitouch \
  libs/armeabi-v7a/minitouch-nopie \
  libs/mips/minitouch \
  libs/mips/minitouch-nopie \
  libs/mips64/minitouch \
  libs/mips64/minitouch-nopie \
  libs/x86/minitouch \
  libs/x86/minitouch-nopie \
  libs/x86_64/minitouch \
  libs/x86_64/minitouch-nopie \

default: prebuilt

clean:
	ndk-build clean
	rm -rf prebuilt

$(NDKBUILT):
	ndk-build

# It may feel a bit redundant to list everything here. However it also
# acts as a safeguard to make sure that we really are including everything
# that is supposed to be there.
prebuilt: \
  prebuilt/arm64-v8a/bin/minitouch \
  prebuilt/arm64-v8a/bin/minitouch-nopie \
  prebuilt/armeabi/bin/minitouch \
  prebuilt/armeabi/bin/minitouch-nopie \
  prebuilt/armeabi-v7a/bin/minitouch \
  prebuilt/armeabi-v7a/bin/minitouch-nopie \
  prebuilt/mips/bin/minitouch \
  prebuilt/mips/bin/minitouch-nopie \
  prebuilt/mips64/bin/minitouch \
  prebuilt/mips64/bin/minitouch-nopie \
  prebuilt/x86/bin/minitouch \
  prebuilt/x86/bin/minitouch-nopie \
  prebuilt/x86_64/bin/minitouch \
  prebuilt/x86_64/bin/minitouch-nopie \

prebuilt/%/bin/minitouch: libs/%/minitouch
	mkdir -p $(@D)
	cp $^ $@

prebuilt/%/bin/minitouch-nopie: libs/%/minitouch-nopie
	mkdir -p $(@D)
	cp $^ $@
