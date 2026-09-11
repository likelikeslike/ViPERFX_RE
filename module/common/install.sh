if service list 2>/dev/null | grep -qi 'android.hardware.audio.effect.IFactory'; then
  ui_print "! AIDL audio effect HAL detected"
  abort "! Use the AIDL module (ViPER4Android-RE-AIDL) instead"
fi

echo -n $LIBPATCH >$MODPATH/libpatch.txt

ui_print "    Copying lib files..."

cp_ch -n $MODPATH/common/files/libv4a_re_$ABI32.so $MODPATH$LIBDIR/lib/soundfx/libv4a_re.so
if [ "$IS64BIT" ]; then
  cp_ch -n $MODPATH/common/files/libv4a_re_$ABI.so $MODPATH$LIBDIR/lib64/soundfx/libv4a_re.so
fi

ui_print "    Patching audio_effects config files"
CFGS="$(find /odm /system /vendor -type f -name "*audio_effects*.conf" -o -name "*audio_effects*.xml")"
for OFILE in ${CFGS}; do
  FILE="$MODPATH$(echo $OFILE | sed "s|^/vendor|/system/vendor|g")"
  cp_ch -n $OFILE $FILE
  case $FILE in
  *.conf)
    sed -i "/v4a_standard_re {/,/}/d" $FILE
    sed -i "/v4a_re {/,/}/d" $FILE
    sed -i "s/^effects {/effects {\n  v4a_standard_re {\n    library v4a_re\n    uuid 90380da3-8536-4744-a6a3-5731970e640f\n  }/g" $FILE
    sed -i "s/^libraries {/libraries {\n  v4a_re {\n    path $LIBPATCH\/lib\/soundfx\/libv4a_re.so\n  }/g" $FILE
    ;;
  *.xml)
    sed -i "/v4a_standard_re/d" $FILE
    sed -i "/v4a_re/d" $FILE
    sed -i "/<libraries>/ a\        <library name=\"v4a_re\" path=\"libv4a_re.so\"\/>" $FILE
    sed -i "/<effects>/ a\        <effect name=\"v4a_standard_re\" library=\"v4a_re\" uuid=\"90380da3-8536-4744-a6a3-5731970e640f\"\/>" $FILE
    ;;
  esac
done
