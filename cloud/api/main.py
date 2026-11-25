import datetime
import hashlib
import hmac
import os
import secrets
import sqlite3
import uuid

from fastapi import Cookie, Depends, FastAPI, File, Header, HTTPException, Response, UploadFile
from fastapi.responses import FileResponse
from pydantic import BaseModel

DATA_DIR = "/data"
DB_PATH = os.path.join(DATA_DIR, "plateforme.db")
FW_DIR = os.path.join(DATA_DIR, "firmware")
AGENT_KEY = os.environ.get("AGENT_KEY", "")
MAX_SIZE = 256 * 1024
DELAI_TRAVAIL = datetime.timedelta(minutes=3)     # au-dela, un travail en cours est abandonne
DELAI_PRESENCE = datetime.timedelta(seconds=20)   # au-dela, la carte est hors ligne
DUREE_SESSION = datetime.timedelta(hours=12)

os.makedirs(FW_DIR, exist_ok=True)


def db():
    con = sqlite3.connect(DB_PATH)
    con.row_factory = sqlite3.Row
    return con


with db() as con:
    con.execute(
        """CREATE TABLE IF NOT EXISTS jobs (
            id TEXT PRIMARY KEY,
            filename TEXT NOT NULL,
            size INTEGER NOT NULL,
            sha256 TEXT NOT NULL,
            status TEXT NOT NULL,
            created_at TEXT NOT NULL,
            started_at TEXT,
            finished_at TEXT,
            chip_id TEXT,
            output TEXT,
            end_reason TEXT,
            error TEXT,
            user_id INTEGER
        )"""
    )
    colonnes = [r["name"] for r in con.execute("PRAGMA table_info(jobs)")]
    if "user_id" not in colonnes:
        con.execute("ALTER TABLE jobs ADD COLUMN user_id INTEGER")
    con.execute(
        """CREATE TABLE IF NOT EXISTS agent (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            last_seen TEXT
        )"""
    )
    con.execute(
        """CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            username TEXT UNIQUE NOT NULL,
            password TEXT NOT NULL,
            admin INTEGER NOT NULL DEFAULT 0,
            created_at TEXT NOT NULL
        )"""
    )
    con.execute(
        """CREATE TABLE IF NOT EXISTS sessions (
            token TEXT PRIMARY KEY,
            user_id INTEGER NOT NULL,
            expires_at TEXT NOT NULL
        )"""
    )


def maintenant():
    return datetime.datetime.now(datetime.timezone.utc)


def now():
    return maintenant().isoformat(timespec="seconds")


def hacher(mot_de_passe, sel=None):
    sel = sel or secrets.token_bytes(16)
    h = hashlib.pbkdf2_hmac("sha256", mot_de_passe.encode(), sel, 200_000)
    return sel.hex() + "$" + h.hex()


def verifier_mot_de_passe(mot_de_passe, stocke):
    sel_hex = stocke.split("$")[0]
    return hmac.compare_digest(hacher(mot_de_passe, bytes.fromhex(sel_hex)), stocke)


def expirer(con):
    limite = (maintenant() - DELAI_TRAVAIL).isoformat(timespec="seconds")
    con.execute(
        "UPDATE jobs SET status = 'echec', finished_at = ?, "
        "error = 'delai depasse : la carte n''a pas repondu' "
        "WHERE status = 'en_cours' AND started_at < ?",
        (now(), limite),
    )


def signaler_presence(con):
    con.execute(
        "INSERT INTO agent (id, last_seen) VALUES (1, ?) "
        "ON CONFLICT(id) DO UPDATE SET last_seen = excluded.last_seen",
        (now(),),
    )


def verifier_id(job_id):
    if not job_id.isalnum():
        raise HTTPException(400, "Identifiant invalide")


app = FastAPI(
    title="Plateforme de programmation STM32",
    docs_url="/api/docs",
    openapi_url="/api/openapi.json",
)


def verifier_agent(x_agent_key: str = Header(default="")):
    if not AGENT_KEY or not hmac.compare_digest(x_agent_key, AGENT_KEY):
        raise HTTPException(401, "Cle d'agent invalide")


def utilisateur(session: str | None = Cookie(default=None)):
    if not session:
        raise HTTPException(401, "Connexion requise")
    with db() as con:
        row = con.execute(
            "SELECT u.id, u.username, u.admin FROM sessions s "
            "JOIN users u ON u.id = s.user_id "
            "WHERE s.token = ? AND s.expires_at > ?",
            (session, now()),
        ).fetchone()
    if row is None:
        raise HTTPException(401, "Session expiree, reconnectez-vous")
    return dict(row)


# ---------- Comptes ----------

class Identifiants(BaseModel):
    username: str
    password: str


@app.post("/api/login")
def connexion(ident: Identifiants, response: Response):
    with db() as con:
        row = con.execute(
            "SELECT id, password FROM users WHERE username = ?", (ident.username,)
        ).fetchone()
        if row is None or not verifier_mot_de_passe(ident.password, row["password"]):
            raise HTTPException(401, "Identifiant ou mot de passe incorrect")
        token = secrets.token_urlsafe(32)
        expire = (maintenant() + DUREE_SESSION).isoformat(timespec="seconds")
        con.execute("DELETE FROM sessions WHERE expires_at < ?", (now(),))
        con.execute(
            "INSERT INTO sessions (token, user_id, expires_at) VALUES (?, ?, ?)",
            (token, row["id"], expire),
        )
    response.set_cookie(
        "session", token, httponly=True, samesite="strict", secure=True,
        max_age=int(DUREE_SESSION.total_seconds()),
    )
    return {"username": ident.username}


@app.post("/api/logout")
def deconnexion(response: Response, session: str | None = Cookie(default=None)):
    if session:
        with db() as con:
            con.execute("DELETE FROM sessions WHERE token = ?", (session,))
    response.delete_cookie("session")
    return {"status": "ok"}


@app.get("/api/me")
def moi(u: dict = Depends(utilisateur)):
    return {"username": u["username"], "admin": bool(u["admin"])}


# ---------- Cote utilisateur ----------

@app.post("/api/jobs", status_code=201)
async def deposer(fichier: UploadFile = File(...), u: dict = Depends(utilisateur)):
    if not fichier.filename.lower().endswith(".bin"):
        raise HTTPException(400, "Seuls les fichiers .bin sont acceptes")
    data = await fichier.read(MAX_SIZE + 1)
    if len(data) == 0 or len(data) > MAX_SIZE:
        raise HTTPException(400, "Taille invalide (256 Ko maximum)")
    job_id = uuid.uuid4().hex[:12]
    with open(os.path.join(FW_DIR, job_id + ".bin"), "wb") as f:
        f.write(data)
    with db() as con:
        con.execute(
            "INSERT INTO jobs (id, filename, size, sha256, status, created_at, user_id) "
            "VALUES (?, ?, ?, ?, 'en_attente', ?, ?)",
            (job_id, fichier.filename, len(data), hashlib.sha256(data).hexdigest(),
             now(), u["id"]),
        )
    return {"id": job_id, "status": "en_attente", "size": len(data)}


@app.get("/api/jobs")
def lister(u: dict = Depends(utilisateur)):
    requete = (
        "SELECT j.id, j.filename, j.size, j.status, j.created_at, us.username "
        "FROM jobs j LEFT JOIN users us ON us.id = j.user_id "
    )
    with db() as con:
        expirer(con)
        if u["admin"]:
            rows = con.execute(requete + "ORDER BY j.created_at DESC LIMIT 50").fetchall()
        else:
            rows = con.execute(
                requete + "WHERE j.user_id = ? ORDER BY j.created_at DESC LIMIT 50",
                (u["id"],),
            ).fetchall()
    return [dict(r) for r in rows]


@app.get("/api/jobs/{job_id}")
def consulter(job_id: str, u: dict = Depends(utilisateur)):
    verifier_id(job_id)
    with db() as con:
        expirer(con)
        row = con.execute("SELECT * FROM jobs WHERE id = ?", (job_id,)).fetchone()
    if row is None or (not u["admin"] and row["user_id"] != u["id"]):
        raise HTTPException(404, "Travail introuvable")
    return dict(row)


@app.get("/api/status")
def etat_carte():
    with db() as con:
        expirer(con)
        occupee = con.execute(
            "SELECT 1 FROM jobs WHERE status = 'en_cours' LIMIT 1"
        ).fetchone()
        en_attente = con.execute(
            "SELECT COUNT(*) FROM jobs WHERE status = 'en_attente'"
        ).fetchone()[0]
        row = con.execute("SELECT last_seen FROM agent WHERE id = 1").fetchone()
    derniere = row["last_seen"] if row else None
    if occupee:
        etat = "occupee"
    elif derniere and maintenant() - datetime.datetime.fromisoformat(derniere) < DELAI_PRESENCE:
        etat = "en_ligne"
    else:
        etat = "hors_ligne"
    return {"etat": etat, "derniere_activite": derniere, "en_attente": en_attente}


# ---------- Cote passerelle ESP32 ----------

@app.get("/api/agent/next", dependencies=[Depends(verifier_agent)])
def prochain():
    with db() as con:
        expirer(con)
        signaler_presence(con)
        row = con.execute(
            "SELECT id, size, sha256 FROM jobs WHERE status = 'en_attente' "
            "ORDER BY created_at LIMIT 1"
        ).fetchone()
        if row is None:
            return Response(status_code=204)
        con.execute(
            "UPDATE jobs SET status = 'en_cours', started_at = ? WHERE id = ?",
            (now(), row["id"]),
        )
    return dict(row)


@app.get("/api/agent/jobs/{job_id}/firmware", dependencies=[Depends(verifier_agent)])
def telecharger(job_id: str):
    verifier_id(job_id)
    chemin = os.path.join(FW_DIR, job_id + ".bin")
    if not os.path.isfile(chemin):
        raise HTTPException(404, "Fichier introuvable")
    return FileResponse(chemin, media_type="application/octet-stream")


class Resultat(BaseModel):
    succes: bool
    chip_id: str | None = None
    output: str = ""
    end_reason: str = ""
    error: str | None = None


@app.post("/api/agent/jobs/{job_id}/result", dependencies=[Depends(verifier_agent)])
def resultat(job_id: str, r: Resultat):
    verifier_id(job_id)
    with db() as con:
        signaler_presence(con)
        cur = con.execute(
            "UPDATE jobs SET status = ?, finished_at = ?, chip_id = ?, output = ?, "
            "end_reason = ?, error = ? WHERE id = ? AND status = 'en_cours'",
            ("termine" if r.succes else "echec", now(), r.chip_id,
             r.output[:20000], r.end_reason, r.error, job_id),
        )
    if cur.rowcount == 0:
        raise HTTPException(409, "Ce travail n'est pas en cours")
    return {"status": "ok"}
