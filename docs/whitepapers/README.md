# White papers

| Paper | Topic | Source |
|---|---|---|
| [What axon's numbers support](axon-performance-claims.pdf) (PDF, 12 pages, 2026-11-21; [HTML](axon-performance/whitepaper.html)) | Performance claims audit: every comparison from PRs #43-#65 (GPU and host vs ROS 2, iceoryx2, the GPU-sharing ROS 2 transports; workloads; Jetson camera → inference; RT), each graded strong / conditional / parity / not a win, with recommended wording | [`axon-performance/`](axon-performance/) |
| [Small cameras, big costs](axon-jetson-camera-inference-e2e.pdf) (PDF, 10 pages, 2026-11, revised 11-21) | Camera-to-inference latency on Jetson AGX Xavier with a USB 2.0 UVC camera and a USB 3 OAK-D: zero-copy against a copy path, stage by stage; first GPU operation after idle and camera timestamp → frame available; guidance for hand-mounted cameras | [`jetson-camera-e2e/`](jetson-camera-e2e/) |

Each paper is built from the raw data in the repository: `make_figures.py` computes every number
into `numbers.json` and draws the figures, and `build.sh` prints the HTML to PDF with headless
Chrome.
