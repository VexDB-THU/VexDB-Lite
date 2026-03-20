"""VexDB-Lite: DuckDB with built-in HNSW vector search.

Usage:
    import vexdb_lite as vex
    con = vex.connect()           # VEX extension auto-loaded
    con = vex.connect('my.db')    # persistent mode

    # All duckdb APIs available
    con.execute("CREATE TABLE t (id INT, vec FLOAT[128])")
    con.execute("CREATE INDEX idx ON t USING GRAPH_INDEX (vec) WITH (metric='cosine')")
"""

import duckdb as _duckdb

# Re-export everything from duckdb
from duckdb import *  # noqa: F401,F403
from duckdb import __version__ as _duckdb_version

__version__ = _duckdb_version


def _ensure_vex(conn):
    """Ensure VEX extension is loaded on a connection."""
    try:
        r = conn.execute(
            "SELECT loaded FROM duckdb_extensions() WHERE extension_name='vex'"
        ).fetchone()
        if not r or not r[0]:
            conn.execute("LOAD vex")
    except Exception:
        try:
            conn.execute("LOAD vex")
        except Exception:
            pass
    return conn


# Wrap connect to auto-load VEX
_original_connect = _duckdb.connect


def connect(*args, **kwargs):
    """Create a DuckDB connection with VEX vector search extension auto-loaded.

    Accepts the same arguments as duckdb.connect().
    """
    conn = _original_connect(*args, **kwargs)
    return _ensure_vex(conn)


# Also provide a default in-memory connection
def default_connection():
    """Get the default in-memory connection with VEX loaded."""
    return _ensure_vex(_duckdb.default_connection())
