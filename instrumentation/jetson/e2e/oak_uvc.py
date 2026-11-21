#!/usr/bin/env python3
# Present an OAK-D colour camera as a standard UVC device (depthai 2.x UVC node) for run_e2e.sh.
# Keep it running while measuring; the UVC device appears as a new /dev/videoN (NV12 only).
# OAK colour camera as a UVC device (depthai 2.x UVC node). Usage: oak_uvc.py [W H [seconds [fps]]]
import sys, time, depthai as dai
W, H = (int(sys.argv[1]), int(sys.argv[2])) if len(sys.argv) > 2 else (1280, 720)
SEC = float(sys.argv[3]) if len(sys.argv) > 3 else 600
p = dai.Pipeline()
cam = p.createColorCamera()
cam.setBoardSocket(dai.CameraBoardSocket.CAM_A)
cam.setResolution(dai.ColorCameraProperties.SensorResolution.THE_800_P)
cam.setInterleaved(False)
if (W, H) != (1280, 800):
    cam.setVideoSize(W, H)
if len(sys.argv) > 4:
    cam.setFps(float(sys.argv[4]))
uvc = p.createUVC()
cam.video.link(uvc.input)
cfg = dai.Device.Config()
cfg.board.uvc = dai.BoardConfig.UVC(W, H)
cfg.board.uvc.frameType = dai.ImgFrame.Type.NV12
p.setBoardConfig(cfg.board)
with dai.Device(p) as dev:
    print("usb speed:", dev.getUsbSpeed(), "mxid:", dev.getMxId(), flush=True)
    t0 = time.time()
    while time.time() - t0 < SEC:
        time.sleep(0.2)
