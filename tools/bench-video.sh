#!/usr/bin/env bash
# Measures how many frames/s N parallel streams reach in different decode +
# GPU-output variants (the same elements CAMAI VMS uses). Run inside the
# graphical session (GL needs the display).
#
#   tools/bench-video.sh <stream-url> [N=8] [seconds=15]
#   URL: http(s)://…/mpegts (best), rtsp://…
set -u
URL="${1:?usage: $0 <stream-url> [N] [seconds]}"
N="${2:-8}"
SEC="${3:-15}"

src() {
  case "$URL" in
    rtsp*) echo "rtspsrc location=$URL latency=200 ! rtph264depay ! h264parse" ;;
    *)     echo "souphttpsrc location=$URL is-live=true ! tsdemux ! h264parse" ;;
  esac
}

run() {   # name decoder-chain output-chain
  local name="$1" dec="$2" out="$3" p=""
  for i in $(seq 1 "$N"); do
    p="$p $(src) ! $dec ! queue max-size-buffers=2 leaky=downstream ! $out ! fpsdisplaysink name=S$i video-sink=fakesink text-overlay=false sync=false"
  done
  timeout -s INT "$SEC" gst-launch-1.0 -v $p > /tmp/bench.$$.txt 2>&1
  local total err
  total=$(grep -oE "Sink:S[0-9]+: last-message = rendered: [0-9]+, dropped: [0-9]+, current: [0-9.]+" /tmp/bench.$$.txt \
          | awk -F'[:,]' '{n=$2; sub(/ /,"",n); last[n]=$NF} END {for (k in last) s+=last[k]; printf "%.0f", s}')
  err=$(grep -m1 -oE "(ERROR|ОШИБКА).*" /tmp/bench.$$.txt | cut -c1-110)
  printf "%-48s %6s к/с  %s\n" "$name" "${total:-0}" "${err:+[$err]}"
  rm -f /tmp/bench.$$.txt
}

GL='glupload ! glcolorconvert ! video/x-raw(memory:GLMemory),format=RGBA'
echo "Потоков: $N, длительность: $SEC с, ожидается: $N × частота кадров камеры"
echo "------------------------------------------------------------------------"
run "1. MPP: только декодирование"            "mppvideodec"                              "identity"
run "2. MPP → GPU (как сейчас в программе)"   "mppvideodec format=NV12"                  "$GL"
run "3. MPP → GPU, dma-feature (DMA-BUF)"     "mppvideodec format=NV12 dma-feature=true" "$GL"
run "4. MPP+RGA 640×360 → GPU"                "mppvideodec format=NV12 width=640 height=360" "$GL"
run "5. MPP+RGA 640×360 → GPU, dma-feature"   "mppvideodec format=NV12 width=640 height=360 dma-feature=true" "$GL"
run "6. Программный декодер → GPU"            "avdec_h264"                               "$GL"
