# Architecture Decision Records

Short records of the load-bearing design decisions in dczc, each with the
**measured evidence** that backs it. ADRs capture *why* a choice was made so it can
be revisited deliberately, not by accident.

Format: Status · Context · Decision · Consequences · Evidence. Source of truth for the
design remains [`DesignFiles/`](../../DesignFiles/); ADRs record decisions and link the
measurements that validated them ([docs/hardware-verification.md](../hardware-verification.md),
[benchmarks/](../../benchmarks/)).

| # | Decision | Status |
|---|---|---|
| [0001](0001-data-centric-zero-copy-over-dds.md) | Data-centric zero-copy instead of ROS2/DDS serialization | Accepted |
| [0002](0002-fd-sidecar-scm-rights.md) | Deliver FDs via a SCM_RIGHTS sidecar, separate from the metadata queue | Accepted |
| [0003](0003-metadata-backend-pluggable.md) | Pluggable metadata backend: seqlock default, Iceoryx2 optional | Accepted |
| [0004](0004-latest-value-rt-consumer.md) | Latest-value-wins RT consumer (poll), inference off the RT path | Accepted |
