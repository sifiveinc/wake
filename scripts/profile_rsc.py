# Copyright 2024 SiFive, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You should have received a copy of LICENSE.Apache2 along with
# this software. If not, you may obtain a copy at
#
#    https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
import argparse
import urllib3
import sys
import json
import time

def make_add_job_json(size, blob_id):
    visible_files = [{
        "path": "vis." + blob_id + "." + str(i),
        "hash": "536246ae5ed7e59ee3593656144644f2e733a9d86c7e54730c44a5fe9eff246c"
    } for i in range(size) ]

    output_files = [{
        "path": "out." + blob_id +  "." + str(i),
        "mode": 420,
        "blob_id": blob_id
    } for i in range(size) ]

    return {
        "cmd": [47,117,115,114],
        "cwd":".",
        "env":[80,65,84,72,61,47],
        "hidden_info":"",
        "is_atty": True,
        "stdin":"",
        "visible_files": visible_files,
        "output_dirs": [],
        "output_symlinks": [],
        "output_files": output_files,
        "stdout_blob_id": blob_id,
        "stderr_blob_id": blob_id,
        "status":0,
        "runtime":0.48949799999999999,
        "cputime":0.45690199999999997,
        "memory":102068224,
        "ibytes":209192,
        "obytes":292123,
        "label":"profiling job"
    }

def add_job_step(http, args, blob_id, size):
    print("Profiling POST /job with", size, "visible and output files:")
    json_body = json.dumps(make_add_job_json(size, blob_id))

    start = time.time()
    res = http.request(
        "POST", 
        args.server + "/job", 
        headers={ 'Authorization': args.auth, 'Content-Type': 'application/json' },
        body=json_body
    )
    end = time.time()

    elapsed_ms = int((end - start) * 1000)

    print("   ", elapsed_ms, "ms elapsed")
    print("   ", round(elapsed_ms / size, 5), "ms per visible/output file")
    print("")

def add_job(http, args):
    # Create a backing blob
    res = http.request(
        "POST", 
        args.server + "/blob", 
        headers={ 'Authorization': args.auth },
        fields={ 'file': "dummy blob" }
    )

    if res.status != 200:
        print("Failed to create a dummy blob")
        sys.exit(2)

    blob_id = json.loads(res.data)["blobs"][0]["id"]



    add_job_step(http, args, blob_id, 1)
    add_job_step(http, args, blob_id, 10)
    add_job_step(http, args, blob_id, 100)
    add_job_step(http, args, blob_id, 1000)
    add_job_step(http, args, blob_id, 10000)
    add_job_step(http, args, blob_id, 100000)
    add_job_step(http, args, blob_id, 1000000)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Profile the remote shared cache')
    parser.add_argument('--func', help='which profile func to run', required=True)
    parser.add_argument('--server', help='fully quailified url to connect to server', required=True)
    parser.add_argument('--auth', help='server authorization key', required=True)
    args = parser.parse_args()

    http = urllib3.PoolManager()

    try:
        res = http.request("POST", args.server + "/auth/check", headers={'Authorization': args.auth})
    except:
        print('Unable to establish connection')
        sys.exit(2)
      
    if res.status != 200:
        print('Invalid authorization')
        sys.exit(2)

    if args.func != 'add_job':
        print('Unknown profile function')
        sys.exit(2)

    add_job(http, args)

