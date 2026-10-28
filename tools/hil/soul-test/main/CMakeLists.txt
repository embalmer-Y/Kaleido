idf_component_register(SRCS "main.c" "../../../../components/kaleido_drive/test/test_kaleido_drive.c" "../../../../components/kaleido_emotion/test/test_kaleido_emotion.c" "../../../../components/kaleido_ignition/test/test_kaleido_ignition.c"
                       INCLUDE_DIRS "."
                       REQUIRES kaleido_drive kaleido_emotion kaleido_ignition unity)
