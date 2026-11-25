import getpass
import sqlite3
import sys

from main import db, hacher, now


def usage():
    print("Usage : python manage.py ajouter <nom> [--admin]")
    print("        python manage.py supprimer <nom>")
    print("        python manage.py lister")
    sys.exit(1)


if len(sys.argv) < 2:
    usage()

commande = sys.argv[1]

with db() as con:
    if commande == "ajouter" and len(sys.argv) >= 3:
        nom = sys.argv[2]
        admin = "--admin" in sys.argv
        mdp = getpass.getpass("Mot de passe : ")
        if len(mdp) < 8:
            print("Erreur : 8 caracteres minimum")
            sys.exit(1)
        if mdp != getpass.getpass("Confirmation : "):
            print("Erreur : les deux mots de passe sont differents")
            sys.exit(1)
        try:
            con.execute(
                "INSERT INTO users (username, password, admin, created_at) VALUES (?, ?, ?, ?)",
                (nom, hacher(mdp), int(admin), now()),
            )
        except sqlite3.IntegrityError:
            print("Erreur : ce nom existe deja")
            sys.exit(1)
        print("Compte " + nom + " cree" + (" (administrateur)" if admin else ""))

    elif commande == "supprimer" and len(sys.argv) >= 3:
        row = con.execute("SELECT id FROM users WHERE username = ?", (sys.argv[2],)).fetchone()
        if row is None:
            print("Erreur : compte introuvable")
            sys.exit(1)
        con.execute("DELETE FROM sessions WHERE user_id = ?", (row["id"],))
        con.execute("DELETE FROM users WHERE id = ?", (row["id"],))
        print("Compte supprime")

    elif commande == "lister":
        for r in con.execute("SELECT username, admin, created_at FROM users ORDER BY username"):
            print(r["username"], "(administrateur)" if r["admin"] else "", r["created_at"])

    else:
        usage()
