# Verify SDK directories are present (run setup_sdk.sh once after cloning)
if [[ ! -d "runtime/librknn_api" || ! -d "3rdparty/rga/include" ]]; then
  echo "ERROR: SDK directories missing. Run the following first:"
  echo "  bash setup_sdk.sh"
  exit 1
fi

cd ./yolov8n_cap_multithread
bash build.sh
scp -r install/yolov8n_cap_multithread/ khadas@192.168.1.58:~/programs/