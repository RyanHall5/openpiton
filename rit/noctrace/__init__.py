"""noctrace: parse and analyze NoC flit traffic from simulation output."""
from .adapters import load_events, ADAPTERS
from .model import Run, Mesh


def load_run(path, fmt="auto", **opts):
    """Load one simulation output file and reconstruct its traffic."""
    events, meta = load_events(path, fmt, **opts)
    return Run(events, meta)
