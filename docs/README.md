# Documentation

Start with the document written for you.

| You are | Read |
|---|---|
| An operator or supervisor | [User Manual](USER_MANUAL.md) |
| Installing or looking after a panel | [Install and Operations Guide](INSTALL-AND-OPERATIONS.md) |
| An engineer evaluating the system | [Architecture](architecture/ARCHITECTURE.md), then [Benchmarks](benchmarks/README.md) and [Known limitations](KNOWN-LIMITATIONS.md) |
| A developer | [Where the code starts](code/ENTRY-POINTS.md), then [Architecture](architecture/ARCHITECTURE.md) |
| Responsible for security | [Security policy](SECURITY.md) and [Threat model](THREAT-MODEL.md) |
| Checking compliance | [Standards traceability](STANDARDS.md) |

## All documents

| Document | Contents |
|---|---|
| [USER_MANUAL.md](USER_MANUAL.md) | every screen, signing in, operating devices, alarms, EMERGENCY, configuration, troubleshooting |
| [INSTALL-AND-OPERATIONS.md](INSTALL-AND-OPERATIONS.md) | install, commission, files and backups, running, health checks, upgrade, recovery |
| [architecture/ARCHITECTURE.md](architecture/ARCHITECTURE.md) | the whole runtime and each custom part, with diagrams: threads, tag table, fieldbus, alarm engine, renderer, security |
| [code/ENTRY-POINTS.md](code/ENTRY-POINTS.md) | a guided tour of the source: where main, the renderer, the fieldbus loop and the alarm engine start |
| [design/HMI-DESIGN.md](design/HMI-DESIGN.md) | operator interface rules (ISA-101), layouts at any device count, touch targets, latency budget |
| [design/ALARMS.md](design/ALARMS.md) | alarm management, item by item against ISA-18.2 |
| [SECURITY.md](SECURITY.md) | security controls, what the site must do, how to report a vulnerability |
| [THREAT-MODEL.md](THREAT-MODEL.md) | assets, trust boundaries, threats and their controls |
| [STANDARDS.md](STANDARDS.md) | each standard, where it is implemented, and how it is checked |
| [adr/](adr/README.md) | design decisions and why they were made |
| [benchmarks/README.md](benchmarks/README.md) | measured performance, the method, and the comparison with LVGL |
| [KNOWN-LIMITATIONS.md](KNOWN-LIMITATIONS.md) | what is not done, not verified or not supported |
| [LAB_DEPLOYMENT.md](LAB_DEPLOYMENT.md) | which lab panels can run the HMI |
| [evidence/](evidence/) | measurements and test results for this build |

**Every document says what is verified and what is not.** A claim without a measurement or a
test says so.
