#!/usr/bin/python3

import json
import logging
import sys
import os
import subprocess
import datetime

from .common import DEFAULT_LOG_FORMAT, DEFAULT_LOG_LEVEL

from threading import Lock

MODULE_NAME = "hive-utils.hive-node"

logger = logging.getLogger(MODULE_NAME)
logger.setLevel(DEFAULT_LOG_LEVEL)

ch = logging.StreamHandler(sys.stdout)
ch.setLevel(DEFAULT_LOG_LEVEL)
ch.setFormatter(logging.Formatter(DEFAULT_LOG_FORMAT))

logger.addHandler(ch)

from typing import NamedTuple

class HiveNode(object):
  hived_binary = None
  hived_process = None
  hived_lock = Lock()
  hived_data_dir = None
  hived_args = list()

  def __init__(self, binary_path : str, working_dir : str, binary_args : list):
    logger.info("New hive node")
    if not os.path.exists(binary_path):
      raise ValueError("Path to hived binary is not valid.")
    if not os.path.isfile(binary_path):
      raise ValueError("Path to hived binary must point to file")
    self.hived_binary = binary_path

    if not os.path.exists(working_dir):
      raise ValueError("Path to data directory is not valid")
    if not os.path.isdir(working_dir):
      raise ValueError("Data directory is not valid directory")
    self.hived_data_dir = working_dir

    if binary_args:
      self.hived_args.extend(binary_args)

  def __enter__(self):
    self.hived_lock.acquire()

    from subprocess import Popen, PIPE
    from time import sleep

    hived_command = [
      self.hived_binary,
      "--data-dir={}".format(self.hived_data_dir)
    ]
    hived_command.extend(self.hived_args)

    self.hived_process = Popen(hived_command, stdout=None, stderr=None)
    self.hived_process.poll()
    sleep(5)

    if self.hived_process.returncode:
      raise Exception("Error during starting node")

  def __exit__(self, exc, value, tb):
    logger.info("Closing node")
    from signal import SIGINT, SIGTERM
    from time import sleep

    if self.hived_process is not None:
      self.hived_process.poll()
      if self.hived_process.returncode != 0:
        self.hived_process.send_signal(SIGINT)
        sleep(7)
        self.hived_process.poll()
        if self.hived_process.returncode != 0:
          self.hived_process.send_signal(SIGTERM)
          sleep(7)
          self.hived_process.poll()
          if self.hived_process.returncode != 0:
            raise Exception("Error during stopping node. Manual intervention required.")
    self.hived_process = None
    self.hived_lock.release()

class HiveNodeInScreen(object):
  def __init__(self, steem_executable, working_dir, config_src_path):
    self.steem_executable = steem_executable
    self.working_dir = working_dir
    self.config_src_path = config_src_path

    from shutil import rmtree, copy
    # remove old data from node
    if os.path.exists(self.working_dir):
      rmtree(self.working_dir)
    os.makedirs(self.working_dir+"/blockchain")
    # copy config file to working dir
    copy(self.config_src_path, self.working_dir + "/config.ini")

    self.steem_config = self.parse_node_config_file(self.working_dir + "/config.ini")
    self.ip_address, self.port = self.steem_config["webserver-http-endpoint"][0].split(":")
    self.ip_address = "http://{}".format(self.ip_address)
    self.node_running = False

  def get_from_config(self, key):
    return self.steem_config.get(key, None)

  def get_node_url(self):
    return "{}:{}/".format(self.ip_address, self.port)

  def is_running(self):
    return self.node_running

  def parse_node_config_file(self, config_file_name):
    ret = dict()
    lines = None
    with open(config_file_name, "r") as f:
      lines = f.readlines()

    for line in lines:
      proc_line = line.strip()
      if proc_line:
        if proc_line.startswith("#"):
            continue
        k, v = proc_line.split("=", 1)
        k = k.strip()
        v = v.strip()
        if k in ret:
          ret[k].append(v)
        else:
          ret[k] = [v]
    return ret

  def run_hive_node(self, additional_params = [], wait_for_blocks = True):
    from .common import detect_process_by_name, save_screen_cfg, save_pid_file, wait_n_blocks, wait_for_string_in_file, kill_process
    detect_process_by_name("steem", self.ip_address, self.port)

    logger.info("*** START NODE at {0}:{1} in {2}".format(self.ip_address, self.port, self.working_dir))

    parameters = [
      self.steem_executable,
      "-d",
      self.working_dir,
      "--advanced-benchmark",
      "--sps-remove-threshold",
      "-1"
    ]

    parameters = parameters + additional_params
    
    self.pid_file_name = "{0}/run_steem-{1}.pid".format(self.working_dir, self.port)
    current_time_str = datetime.datetime.now().strftime("%Y-%m-%d")
    log_file_name = "{0}/{1}-{2}-{3}.log".format(self.working_dir, "steem", self.port, current_time_str)
    screen_cfg_name = "{0}/steem_screen-{1}.cfg".format(self.working_dir, self.port)

    save_screen_cfg(screen_cfg_name, log_file_name)
    screen_params = [
      "screen",
      "-m",
      "-d",
      "-L",
      "-c",
      screen_cfg_name,
      "-S",
      "{0}-{1}-{2}".format("steem", self.port, current_time_str)
    ]

    parameters = screen_params + parameters
    logger.info("Running steemd with command: {0}".format(" ".join(parameters)))
      
    try:
      subprocess.Popen(parameters)
      save_pid_file(self.pid_file_name, "steem", self.port, current_time_str)
      if wait_for_blocks:
        wait_n_blocks("{}:{}".format(self.ip_address, self.port), 5)
      else:
        wait_for_string_in_file(log_file_name, "start listening for ws requests", 240.)
      self.node_running = True
      logger.info("Node at {0}:{1} in {2} is up and running...".format(self.ip_address, self.port, self.working_dir))
    except Exception as ex:
      logger.error("Exception during steemd run: {0}".format(ex))
      kill_process(self.pid_file_name, "steem", self.ip_address, self.port)
      self.node_running = False


  def stop_hive_node(self):
    from .common import kill_process
    logger.info("Stopping node at {0}:{1}".format(self.ip_address, self.port))
    kill_process(self.pid_file_name, "steem", self.ip_address, self.port)
    self.node_running = False

  def __enter__(self):
    self.run_hive_node()

  def __exit__(self, exc, value, tb):
    self.stop_hive_node()

if __name__ == "__main__":
  KEEP_GOING = True

  def sigint_handler(signum, frame):
    logger.info("Shutting down...")
    global KEEP_GOING
    from time import sleep
    KEEP_GOING = False
    sleep(3)
    sys.exit(0)

  def main():
    try:
      import signal
      signal.signal(signal.SIGINT, sigint_handler)

      plugins = ["chain","p2p","webserver","json_rpc","debug_node"]
      config = "# Simple config file\n" \
        + "shared-file-size = 1G\n" \
        + "enable-stale-production = true\n" \
        + "p2p-endpoint = 127.0.0.1:2001\n" \
        + "webserver-http-endpoint = 127.0.0.1:8095\n" \
        + "webserver-ws-endpoint = 127.0.0.1:8096\n" \
        + "plugin = witness debug_node {}\n".format(" ".join(plugins)) \
        + "plugin = database_api debug_node_api block_api\n" \
        + "witness = \"initminer\"\n" \
        + "private-key = 5JNHfZYKGaomSFvd4NUdQ9qMcEAC43kujbfjueTHpVapX1Kzq2n\n" \
        + "required-participation = 0"

      binary_path = "/home/dariusz-work/Builds/hive/programs/steemd/steemd"
      work_dir = "/home/dariusz-work/hive-data"

      print(config)

      with open(work_dir + "/config.ini", "w") as conf_file:
        conf_file.write(config)

      node = HiveNode(binary_path, work_dir, [])
      from time import sleep
      from .common import wait_n_blocks, debug_generate_blocks
      with node:
        print("Waiting 10 blocks")
        wait_n_blocks("http://127.0.0.1:8095", 10)
        print("Done...")
        print(debug_generate_blocks("http://127.0.0.1:8095", "5JHNbFNDg834SFj8CMArV6YW7td4zrPzXveqTfaShmYVuYNeK69", 100))
        while(KEEP_GOING):
          sleep(1)
    except Exception as ex:
      logger.exception("Exception: {}".format(ex))
      sys.exit(1)
  
  main()



