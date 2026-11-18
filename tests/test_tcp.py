"""Интеграционные проверки TCP: реальные процессы и произвольные границы recv."""
import contextlib
import pathlib
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

SERVER, CLIENT, OBSERVER = map(lambda p: str(pathlib.Path(p).resolve()), sys.argv[1:4])
del sys.argv[1:4]
JOIN, CONFIG, START, HUNGRY, EAT, FINISH, CONTINUE, STOP, OBSERVER_JOIN, SNAPSHOT = range(1, 11)


def packet(kind, identity, value=0):
    return struct.pack("!6I", 1, kind, identity, value, 0, 0)


def receive(sock):
    data = bytearray()
    while len(data) < 24:
        chunk = sock.recv(24 - len(data))
        if not chunk:
            raise AssertionError("Соединение закрыто до полного сообщения")
        data.extend(chunk)
    return struct.unpack("!6I", data)


class TcpTests(unittest.TestCase):
    def setUp(self):
        self.resources = contextlib.ExitStack()
        self.addCleanup(self.resources.close)
        self.processes = []
        self.addCleanup(self.stop_processes)

    def stop_processes(self):
        for process in self.processes:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=3)

    def launch(self, args):
        output = self.resources.enter_context(tempfile.TemporaryFile(mode="w+"))
        process = subprocess.Popen(args, stdout=output, stderr=output)
        self.processes.append(process)
        return process, output

    def text(self, output):
        output.seek(0)
        return output.read()

    def server(self, count=2, cycles=2, think=0, eat=20, max_wait=1000, limit=5000):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.port = str(probe.getsockname()[1])
        process, output = self.launch([
            SERVER, "127.0.0.1", self.port, str(count), str(cycles), str(think),
            str(think), str(eat), str(eat), str(max_wait), "0", "42", "fifo", str(limit)])
        deadline = time.monotonic() + 3
        while "ГОТОВО" not in self.text(output):
            self.assertIsNone(process.poll(), self.text(output))
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.005)
        return process, output

    def clients(self, count):
        return [self.launch([CLIENT, "127.0.0.1", self.port, str(i)])
                for i in range(1, count + 1)]

    def codes(self, processes, expected):
        for process, output in processes:
            self.assertEqual(process.wait(timeout=15), expected, self.text(output))

    def raw(self, identity, fragmented=False):
        sock = self.resources.enter_context(socket.create_connection(("127.0.0.1", int(self.port)), 2))
        data = packet(JOIN, identity)
        if fragmented:
            sock.sendall(data[:3])
            # Сервер уже должен обслуживать другие сокеты, пока JOIN неполон.
            self.assertIsNone(self.processes[0].poll())
            time.sleep(.02)
            sock.sendall(data[3:])
        else:
            sock.sendall(data)
        return sock

    def test_cycles_fifo_and_fork_ownership(self):
        for count in (2, 5):
            with self.subTest(count=count):
                server, output = self.server(count=count, cycles=3)
                clients = self.clients(count)
                self.codes([(server, output)] + clients, 0)
                owners, queue, meals = {}, [], 0
                for line in self.text(output).splitlines():
                    hungry = re.search(r"философ (\d+) сообщил о голоде", line)
                    if hungry:
                        queue.append(int(hungry[1]))
                    granted = re.search(r"выдал философу (\d+) вилки (\d+) и (\d+)", line)
                    if granted:
                        identity, left, right = map(int, granted.groups())
                        self.assertEqual(queue.pop(0), identity)
                        self.assertNotIn(left, owners)
                        self.assertNotIn(right, owners)
                        owners[left] = owners[right] = identity
                    freed = re.search(r"философ (\d+) сообщил об окончании еды; освободил вилки (\d+) и (\d+)", line)
                    if freed:
                        identity, left, right = map(int, freed.groups())
                        self.assertEqual(owners.pop(left), identity)
                        self.assertEqual(owners.pop(right), identity)
                        meals += 1
                self.assertEqual(meals, count * 3)
                self.assertFalse(owners or queue)
                for _, client_output in clients:
                    self.assertEqual(self.text(client_output).count("закончил есть"), 3)

    def test_fragmented_frames_and_parallel_non_neighbours(self):
        server, output = self.server(count=5, cycles=1, eat=300)
        sockets = [self.raw(i, fragmented=(i == 1)) for i in range(1, 6)]
        for sock in sockets:
            self.assertEqual(receive(sock)[1], CONFIG)
            self.assertEqual(receive(sock)[1], START)
        sockets[0].sendall(packet(HUNGRY, 1))
        self.assertEqual(receive(sockets[0])[1], EAT)
        sockets[2].sendall(packet(HUNGRY, 3))
        self.assertEqual(receive(sockets[2])[1], EAT)
        # Оба разрешения получены до FINISH первого философа.
        server.send_signal(signal.SIGTERM)
        self.assertEqual(server.wait(timeout=3), 143, self.text(output))
        for sock in sockets:
            self.assertEqual(receive(sock)[1:4:2], (STOP, 143))

    def test_duplicate_id_does_not_stop_server(self):
        server, output = self.server(think=100)
        clients = [self.launch([CLIENT, "127.0.0.1", self.port, "1"])]
        deadline = time.monotonic() + 2
        while "подключён философ 1" not in self.text(output):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.005)
        duplicate = self.launch([CLIENT, "127.0.0.1", self.port, "1"])
        self.codes([duplicate], 1)
        clients.append(self.launch([CLIENT, "127.0.0.1", self.port, "2"]))
        self.codes([(server, output)] + clients, 0)

    def test_observer_receives_complete_state(self):
        server, output = self.server(count=2, cycles=1, eat=30)
        observer = self.launch([OBSERVER, "127.0.0.1", self.port])
        clients = self.clients(2)
        self.codes([(server, output)] + clients + [observer], 0)
        text = self.text(observer[1])
        self.assertIn("Наблюдатель: подключён", text)
        self.assertIn("P1=", text)
        self.assertIn("F1=", text)

    def test_observer_can_join_running_model(self):
        server, output = self.server(count=2, cycles=1, think=100)
        clients = self.clients(2)
        deadline = time.monotonic() + 3
        while "начал размышление" not in self.text(clients[0][1]):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.005)
        observer = self.launch([OBSERVER, "127.0.0.1", self.port])
        self.codes([(server, output)] + clients + [observer], 0)
        self.assertIn("Наблюдатель: подключён", self.text(observer[1]))

    def test_limits(self):
        for options in ({"think": 500, "limit": 30}, {"eat": 500, "max_wait": 30}):
            with self.subTest(options=options):
                server = self.server(**options)
                self.codes([server] + self.clients(2), 2)

    def test_client_death_stops_session(self):
        server = self.server(think=2000)
        clients = self.clients(2)
        deadline = time.monotonic() + 3
        while "начал размышление" not in self.text(clients[0][1]):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.005)
        clients[0][0].kill()
        self.codes([server, clients[1]], 1)

    def test_server_death_detected_by_clients(self):
        server, output = self.server(think=2000)
        clients = self.clients(2)
        deadline = time.monotonic() + 3
        while "начал размышление" not in self.text(clients[0][1]):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.005)
        server.kill()
        self.codes(clients, 1)

    def test_early_finish_and_coalesced_messages_rejected(self):
        server, output = self.server(eat=2000)
        first, second = self.raw(1), self.raw(2)
        for sock in (first, second):
            self.assertEqual(receive(sock)[1], CONFIG)
            self.assertEqual(receive(sock)[1], START)
        first.sendall(packet(HUNGRY, 1) + packet(FINISH, 1))
        self.assertEqual(receive(first)[1], EAT)
        self.assertEqual(receive(first)[1:4:2], (STOP, 1))
        self.assertEqual(server.wait(timeout=3), 1, self.text(output))
        self.assertIn("цикл не засчитан", self.text(output))

    def test_no_clients_timeout(self):
        server, output = self.server()
        self.assertEqual(server.wait(timeout=13), 1, self.text(output))
        self.assertIn("10 секунд", self.text(output))

    def test_registered_client_wrong_id_and_version(self):
        for wrong_version in (False, True):
            with self.subTest(wrong_version=wrong_version):
                server, output = self.server()
                first, second = self.raw(1), self.raw(2)
                for sock in (first, second):
                    self.assertEqual(receive(sock)[1], CONFIG)
                    self.assertEqual(receive(sock)[1], START)
                first.sendall(struct.pack("!6I", 2 if wrong_version else 1,
                                          HUNGRY, 1 if wrong_version else 2, 0, 0, 0))
                self.assertEqual(receive(second)[1:4:2], (STOP, 1))
                self.assertEqual(server.wait(timeout=3), 1, self.text(output))

    def test_client_receives_fragmented_and_coalesced_replies(self):
        listener = self.resources.enter_context(socket.socket())
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(2)
        self.port = str(listener.getsockname()[1])
        client = self.launch([CLIENT, "127.0.0.1", self.port, "1"])
        sock, _ = listener.accept()
        self.resources.enter_context(sock)
        sock.settimeout(2)
        self.assertEqual(receive(sock)[1], JOIN)
        sock.sendall(struct.pack("!6I", 1, CONFIG, 1, 0, 2, 1) + packet(START, 1))
        self.assertEqual(receive(sock)[1], HUNGRY)
        reply = packet(EAT, 1, 1)
        sock.sendall(reply[:5])
        time.sleep(.02)
        sock.sendall(reply[5:])
        self.assertEqual(receive(sock)[1], FINISH)
        sock.sendall(packet(STOP, 1))
        self.codes([client], 0)

    def test_invalid_arguments(self):
        for program, arguments in (
            (SERVER, ["127.0.0.1", "0"]), (SERVER, ["bad", "1234"]),
            (SERVER, ["127.0.0.1", "65536"]), (SERVER, ["127.0.0.1", "1234", "1"]),
            (CLIENT, ["127.0.0.1", "1234", "0"]), (CLIENT, ["127.0.0.1", "1234", "-1"]),
            (CLIENT, ["127.0.0.1", "1234", "257"]), (CLIENT, ["127.0.0.1", "1234", "1abc"]),
        ):
            with self.subTest(arguments=arguments):
                process = subprocess.run([program] + arguments, capture_output=True, timeout=2)
                self.assertEqual(process.returncode, 1)


if __name__ == "__main__":
    unittest.main()
