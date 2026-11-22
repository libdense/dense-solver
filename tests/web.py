"""Exercise the native workbench over loopback HTTP. Run after make web."""
import csv
import http.client
import io
from pathlib import Path
import socket
import subprocess
from urllib.parse import urlencode

ROOT = Path(__file__).resolve().parents[1]


def main():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    process = subprocess.Popen([ROOT/"build/dense-solver-web",str(port)],cwd=ROOT,stdout=subprocess.PIPE)
    try:
        assert process.stdout.readline().startswith(b"dense-solver at")
        def request(path, fields=None, raw=None, content_type="application/x-www-form-urlencoded"):
            conn = http.client.HTTPConnection("127.0.0.1",port,timeout=15)
            body = urlencode(fields).encode() if fields is not None else raw
            conn.request("GET" if body is None else "POST",path,body,{"Content-Type":content_type})
            resp = conn.getresponse()
            status, text = resp.status, resp.read().decode()
            conn.close()
            return status,text

        def get(path):
            status,text = request(path)
            assert status == 200, (path,status)
            return text

        def post(path,fields):
            assert request(path,fields)[0] == 303, path

        settings={"engine":"reasoned","heuristic":"activity","luby":"64","interval":"2000",
                  "decisions":"0","conflicts":"0","seed":"731","restarts":"on","reduce":"on"}
        assert "s UNSATISFIABLE" in get("/")
        post("/preset",{"id":"1"})
        assert "c counting chain" in get("/output")
        assert "At-most-one groups" in get("/structure")
        post("/generate",{"p":"5","h":"4","encoding":"pairwise"})
        assert "p cnf 20 " in get("/")
        text="p cnf 1 1\n1 0\n"
        body=b'--separator\r\nContent-Disposition: form-data; name="file"; filename="sample.cnf"\r\n\r\n'+text.encode()+b'\r\n--separator--\r\n'
        assert request("/upload",raw=body,content_type="multipart/form-data; boundary=separator")[0] == 303
        board = (ROOT / "instances/chessboard12.cnf").read_text()
        post("/solve",dict(settings,cnf=board))
        assert "Hall matching" in get("/structure") and "c method hall" in get("/output")
        post("/solve",dict(settings,cnf=text))
        assert text in get("/")
        post("/solve",settings)
        assert "s SATISFIABLE" in get("/output") and "c xor-row" in get("/structure")
        assert request("/save",{"cnf":text}) == (200,text)
        post("/solve",dict(settings,cnf="nonsense"))
        assert "c solver error" in get("/")
        post("/verify",{})
        assert "14/14" in get("/verify")
        post("/live",{"trials":"3"})
        assert "Live native solve" in get("/bench?data=live")
        assert len(list(csv.DictReader(io.StringIO(get("/csv?data=live")))))==18
        table = get("/bench?instance=sc25-fpga-routing").split("<tbody>")[1]
        assert "sc25-fpga-routing" in table and "sc25-scheduling" not in table
        assert "kissat-4.0.4" in table and "dense-solver-0.0.4" in table
        board_table = get("/bench?data=native-v0.0.3&scope=process&instance=chessboard32").split("<tbody>")[1]
        assert "dense-solver-0.0.3" in board_table and "chessboard32" in board_table
        assert "php9x8" not in board_table
        trials = csv.DictReader(io.StringIO(get("/csv?data=native-v0.0.3")))
        assert len({row["instance"] for row in trials}) == 20
        assert "satsuma-iter+kissat" in get("/bench?data=native-v0.0.2&scope=process")
        assert "satsuma-iter+kissat" not in get("/bench?data=native-v0.0.2&scope=solve").split("<tbody>")[1]
        assert "satsuma-iter+kissat-competition" in get("/bench?data=satsuma-v0.0.1&scope=process")
        assert request("/generate",{"p":101,"h":2,"encoding":"pairwise"})[0]==400
        print("Passed native HTTP solve, generation, editing, upload, download, structure, verification and benchmark checks.")
    finally:
        process.terminate()
        process.wait(timeout=5)


if __name__ == "__main__":
    main()
