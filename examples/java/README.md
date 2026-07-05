# libroqr Java samples

Build the JNI bindings and the samples:

```
eval "$(scripts/setup_picoquic_deps.sh)"
cmake --preset dev            # ROQR_BUILD_JNI=ON in the dev preset
cmake --build --preset dev
```

Run (point java at the built native lib and jars):

```
JNI_DIR=build/dev/jni
JAR=$(find build/dev -name roqr.jar)
SAMPLES=$(find build/dev -name roqr-samples.jar)

# Publisher gateway on :1935 -> RoQR server 127.0.0.1:4443
java -Djava.library.path=$JNI_DIR -cp "$JAR:$SAMPLES" \
    PublishSample 1935 127.0.0.1 4443
# then: ffmpeg ... -f flv rtmp://127.0.0.1:1935/live/cam

# Player gateway on :1936 <- RoQR server, stream "cam"
java -Djava.library.path=$JNI_DIR -cp "$JAR:$SAMPLES" \
    PlaySample 1936 127.0.0.1 4443 cam
# then: ffplay rtmp://127.0.0.1:1936/live/cam
```

A `roqr-relayd --mode media` (or any RoQR server) must be running at the
RoQR host/port.
