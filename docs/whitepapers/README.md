# White papers

| Paper | Topic | Source |
|---|---|---|
| [Small cameras, big costs](axon-jetson-camera-inference-e2e.pdf) (PDF, 7 pages, 2026-11) | Camera-to-inference latency on Jetson AGX Xavier with a USB 2.0 UVC camera: zero-copy against a copy path, stage by stage; first GPU operation after idle and camera timestamp → frame available; guidance for hand-mounted cameras | [`jetson-camera-e2e/`](jetson-camera-e2e/) |

Each paper is built from the raw data in the repository: `make_figures.py` computes every number
into `numbers.json` and draws the figures, and `build.sh` prints the HTML to PDF with headless
Chrome.
