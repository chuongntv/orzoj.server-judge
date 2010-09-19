# $File: work.py
# $Author: Jiakai <jia.kai66@gmail.com>
# $Date: Sun Sep 19 11:57:06 2010 +0800
#
# This file is part of orzoj
# 
# Copyright (C) <2010>  Jiakai <jia.kai66@gmail.com>
# 
# Orzoj is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
# 
# Orzoj is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with orzoj.  If not, see <http://www.gnu.org/licenses/>.
#

"""threads for waiting for tasks and managing judges"""

import threading, Queue, time, os, os.path, hashlib
from orzoj import log, snc, msg, structures, control, conf, filetrans
from orzoj.server import web

_judge_online = {}
_lock_judge_online = threading.Lock()

_task_queue = Queue.Queue()

_refresh_interval = None
_id_max_len = None

_QUEUE_GET_TIMEOUT = 1

class _internal_error(Exception):
    pass

class _thread_fetch_task(threading.Thread):
    def run(self):
        global _judge_online, _lock_judge_online, _task_queue, _refresh_interval, _id_max_len
        global _QUEUE_GET_TIMEOUT

        while not control.test_termination_flag():
            while True:
                task = web.fetch_task()
                if task is None:
                    break
                _task_queue.put(task, True)

            time.sleep(_refresh_interval)

class thread_work(threading.Thread):
    """wait for tasks and distribute them to judges"""
    def run(self):
        global _judge_online, _lock_judge_online, _task_queue, _refresh_interval, _id_max_len
        global _QUEUE_GET_TIMEOUT

        _thread_fetch_task().start()
        while not control.test_termination_flag():

            try:
                task = _task_queue.get(True, _QUEUE_GET_TIMEOUT)
                judge = None

                with _lock_judge_online:
                    for (i, j) in _judge_online.iteritems():
                        if task.lang in j.lang_supported:
                            if judge is None or j.queue.qsize() < judge.queue.qsize():
                                judge = j

                if judge is None:
                    web.report_no_judge(task)
                else:
                    with _lock_judge_online: # the chosen judge may have just deleted itself, so we have to lock
                        if judge.id in _judge_online:
                            judge.queue.put(task, True)
                        else:
                            _task_queue.put(task, True)
            except Queue.Empty:
                pass

def _get_file_list(path):
    """return a dict containing regular files and their corresponding sha1 digests in the direcory @path.
    return None on error"""
    def _sha1_file(path):
        with open(path, 'rb') as f:
            sha1_ctx = hashlib.sha1()
            while True:
                buf = f.read(sha1_ctx.block_size)
                if not buf:
                    return sha1_ctx.digest()
                sha1_ctx.update(buf)

    try:
        ret = {}
        for i in os.listdir(path):
            pf = os.path.normpath(os.path.join(path, i))
            if os.path.isfile():
                ret[i] = _sha1_file(pf)
        return ret
            
    except OSError as e:
        log.error("failed to obtain file list of {0!r}: [errno {1}] [filename {2!r}]: {3}" .
                format(path, e.errno, e.filename, e.strerror))
        return None
    except Exception as e:
        log.error("failed to obtain file list of {0!r}: {1!r}" .
                format(path, e))
        return None

class thread_new_judge_connection(threading.Thread):
    def __init__(self, sock):
        """serve a new connection, which should be orzoj-judge.
        No exceptions are raised, exit silently on error."""
        threading.Thread.__init__(self)
        self._sock = sock

    def _clean():
        global _judge_online, _lock_judge_online, _task_queue, _refresh_interval, _id_max_len
        global _QUEUE_GET_TIMEOUT

        if self._cur_task != None:
            _task_queue.put(self._cur_task, True)
            self._cur_task = None

        judge = self._judge
        if judge.id != None:
            with _lock_judge_online:
                if judge.id in _judge_online:
                    del _judge_online[judge.id]

        if self._web_registered:
            web.remove_judge(judge)

        try:
            while True:
                task = judge.queue.get_nowait()
                _task_queue.put(task, True)
        except Queue.empty:
            return


    def _solve_task(self):
        def _write_msg(m):
            msg.write_msg(self._snc, m)

        def _write_str(s):
            self._snc.write_str(s)

        def _write_uint32(v):
            self._snc.write_uint32(v)

        def _read_msg():
            return msg.read_msg(self._snc)

        def _read_str():
            return self._snc.read_str()

        def _read_uint32():
            return self._snc.read_uint32()

        def _check_msg(m):
            if m != _read_msg():
                log.warning("message check error.")
                raise _internal_error

        global _judge_online, _lock_judge_online, _task_queue, _refresh_interval, _id_max_len
        global _QUEUE_GET_TIMEOUT

        judge = self._judge
        try:
            task = judge.queue.get(True, _QUEUE_GET_TIMEOUT)
        except Queue.Empty:
            return
        self._cur_task = task

        datalist = _get_file_list(task.prob)
        if datalist is None:
            self._cur_task = None
            log.error("No data for problem {0!r}, task discarded" . format(task.prob))
            web.report_no_data(task)
            return

        _write_msg(msg.PREPARE_DATA)
        _write_str(task.prob)
        _write_uint32(len(datalist))

        for (f, sha1) in datalist.iteritems():
            _write_str(f)
            _write_str(sha1)

        while True:
            m = _read_msg()
            if m == msg.DATA_OK:
                break

            if m == msg.DATA_COMPUTING_SHA1:
                continue

            if m == msg.DATA_ERROR:
                self._cur_task = None
                reason = _read_str()
                log.error("judge {0!r} reports data error [prob: {1!r}]: {2!r}" . 
                        format(judge.id, task.prob, reason))
                web.report_error(task, "data error: {0!r}" . format(reason))
                return

            if m != msg.NEED_FILE:
                log.warning("message check error.")
                raise _internal_error

            fpath = os.path.normpath(os.path.join(task.prob, _read_str()))
            speed = filetrans.send(fpath, self._snc)
            log.info("file transfer speed with judge {0!r}: {1} kb/s" .
                    format(judge.id, speed))

        ncase = _read_uint32()

        case_tl = []
        for i in range(ncase):
            case_tl.append(_read_uint32())


        _write_msg(msg.START_JUDGE)
        _write_str(task.lang)
        _write_str(task.src)
        _write_str(task.input)
        _write_str(task.output)

        while True:
            m = _read_msg()
            if m == msg.START_JUDGE_OK:
                break
            if m != msg.START_JUDGE_WAIT:
                log.warning("message check error.")
                raise _internal_error

        web.report_compiling(task, judge)
        m = msg.read_msg(self._snc, msg.COMPILE_MAX_TIME)

        if m == msg.COMPILE_SUCCEED:
            web.report_compile_success(task)
        else:
            if m != msg.COMPILE_FAIL:
                web.report_error(task, "message check error.")
                log.warning("message check error.")
                raise _internal_error
            web.report_compile_failure(task, _read_str())
            self._cur_task = None
            return

        for i in range(ncase):
            _check_msg(msg.REPORT_CASE)
            result = structures.case_result()
            result.read(self._snc, case_tl[i])
            web.report_case_result(task, result)

        _check_msg(msg.REPORT_JUDGE_FINISH)
        result = structures.prob_result()
        result.read(self._snc)

        web.report_prob_result(task, result)

        self._cur_task = None


    def run(self):
        def _write_msg(m):
            msg.write_msg(self._snc, m)

        def _write_str(s):
            self._snc.write_str(s)

        def _read_msg():
            return msg.read_msg(self._snc)

        def _read_str():
            return self._snc.read_str()

        def _read_uint32():
            return self._snc.read_uint32()

        def _check_msg(m):
            if m != _read_msg():
                log.warning("message check error.")
                raise _internal_error

        global _judge_online, _lock_judge_online, _task_queue, _refresh_interval, _id_max_len
        global _QUEUE_GET_TIMEOUT

        self._cur_task = None
        self._web_registered = False

        judge = structures.judge()
        self._judge = judge
        try:
            self._snc = snc.Snc(self._sock, True)
            _check_msg(msg.HELLO)
            judge.id = _read_str()

            if len(judge.id) > _id_max_len:
                _write_msg(msg.ID_TOO_LONG)
                raise _internal_error

            with _lock_judge_online:
                if judge.id in _judge_online:
                    _write_msg(msg.DUPLICATED_ID)
                    log.warning("another judge declares duplicated id {0!r}" .
                            format(judge.id))
                    raise _internal_error

            if _read_uint32() != msg.PROTOCOL_VERSION:
                log.warning("version check error.")
                _write_msg(msg.ERROR)
                raise _internal_error

            cnt = _read_uint32()
            while cnt:
                cnt -= 1
                judge.lang_supported.add(_read_str())

            _write_msg(msg.CONNECT_OK)

            query_ans = {}
            for i in web.get_query_list():
                _write_msg(msg.QUERY_INFO)
                _write_str(i)
                _check_msg(msg.ANS_QUERY)
                query_ans[i] = _read_str()

            web.register_new_judge(judge, query_ans)
            self._web_registered = True

            with _lock_judge_online:
                _judge_online[judge.id] = judge

            while not control.test_termination_flag():
                self._solve_task()

        except snc.Error:
            log.warning("failed to serve judge because of network error.")
            _clean()
            return
        except _internal_error:
            self._snc.close()
            _clean()
            return
        except web.WebError:
            log.warning("failed to serve judge because of error while communicating with website.")
            _write_msg(msg.ERROR)
            self._snc.close()
            _clean()
            return
        except filetrans.OFTPError:
            log.warning("failed to transfer file to judge {0!r}." .
                    format(self._judge.id))
            self._snc.close()
            _clean()
            return

def _set_refresh_interval(arg):
    global _refresh_interval
    _refresh_interval = float(arg[1])
    if _refresh_interval < 1:
        raise conf.UserError("Option {0} can not be less than 1 second" . format(arg[0]))

def _set_id_max_len(arg):
    global _id_max_len
    _id_max_len = int(arg[1])
    if _id_max_len < 1:
        raise conf.UserError("Option {0} can not be less than 1" . format(arg[0]))

def _set_data_dir(arg):
    os.chdir(arg[1])

conf.simple_conf_handler("RefreshInterval", _set_refresh_interval, default = "2")
conf.simple_conf_handler("JudgeIdMaxLen", _set_id_max_len, default = "20")
conf.simple_conf_handler("DataDir", _set_data_dir)

