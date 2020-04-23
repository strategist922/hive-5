#!/usr/bin/python3

import os
import sys
sys.path.append("../")

import json
from testbase import SimpleJsonTest

if __name__ == "__main__":
  import argparse
  parser = argparse.ArgumentParser()

  parser.add_argument("test_node", type = str, help = "IP address of test node")
  parser.add_argument("ref_node", type = str, help = "IP address of reference node")
  parser.add_argument("work_dir", type = str, help = "Work dir")
  parser.add_argument("author", type = str, help = "Author")
  parser.add_argument("permlink", type = str, help = "Permlink")

  args = parser.parse_args()
  tester = SimpleJsonTest(args.test_node, args.ref_node, args.work_dir)

  print("Test node: {}".format(args.test_node))
  print("Ref node: {}".format(args.ref_node))
  print("Work dir: {}".format(args.work_dir))
  print("Author: {}".format(args.author))
  print("Permlink: {}".format(args.permlink))

  test_args = {
    "jsonrpc": "2.0",
    "id": 1,
    "method": "tags_api.get_active_votes",
    "params": { 
      "author": "{}".format(args.author),
      "permlink": "{}".format(args.permlink)
    }
  }

  if tester.compare_results(test_args, True):
    exit(0)
  exit(1)

