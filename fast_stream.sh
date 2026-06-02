# View both camera streams side-by-side (cam 33 on port 8554, cam 51 on port 8555)
# Launch on the board:
#   cd ~/programs/yolov8n_cap_multithread
#   LD_LIBRARY_PATH=./lib 
#   taskset -c 4-7 ./yolov8n_cap_multithread ./data/model/best_yolov8n_800_relu.rknn 33 8554
#   taskset -c 4-7 ./yolov8n_cap_multithread ./data/model/best_yolov8n_800_relu.rknn 51 8555

gst-launch-1.0 \
  compositor name=comp \
    sink_0::xpos=0    sink_0::ypos=0 \
    sink_1::xpos=1920 sink_1::ypos=0 ! \
  video/x-raw,width=3840,height=1080 ! \
  fpsdisplaysink video-sink=ximagesink text-overlay=true sync=false \
  rtspsrc location=rtsp://192.168.1.58:8554/stream latency=0 ! \
    rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! comp.sink_0 \
  rtspsrc location=rtsp://192.168.1.58:8555/stream latency=0 ! \
    rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! comp.sink_1