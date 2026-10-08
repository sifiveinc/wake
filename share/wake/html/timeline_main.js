/* Copyright 2022 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You should have received a copy of LICENSE.Apache2 along with
 * this software. If not, you may obtain a copy at
 *
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//// HTML and DOM operations ////

// DOM element where the Timeline will be attached
const container = document.getElementById('timeline');

function addNodeArrows(node, added) {
    for (const arrow of allArrows) {
        if ((arrow.id_item_1 === node && !(added.includes(arrow.id_item_2))) ||
            (arrow.id_item_2 === node && !(added.includes(arrow.id_item_1)))) {
            arrowObject.addArrow(arrow);
        }
    }
}

function clearVisibleArrows() {
    while (visibleArrows.length > 0) {
        arrowObject.removeArrow(visibleArrows[visibleArrows.length - 1].id);
    }
}

function updateJobs(jobMap) {
    let survivingJobs = new Set();

    let minStarttime = -1;
    let maxEndtime = -1;
    let times = [];

    for (const [jobID, jobNode] of jobMap.entries()) {
        survivingJobs.add(jobID);
        const oldJob = jobs.get(jobID);
        if (oldJob != null && oldJob.starttime === jobNode.starttime) { // no need to update this job
            continue;
        }

        const job = jobNode.job;
        let newJob = {
            id: jobID,
            group: 0,
            start: jobNode.starttime,
            end: jobNode.endtime
        }
        let label = job.label;
        newJob.content = (!(label === "") ? label : jobID.toString());

        newJob.title = jobID + "<br>" + (!(label === "") ? label : "");
        jobs.update(newJob);

        if (minStarttime > jobNode.starttime || minStarttime === -1) {
            minStarttime = jobNode.starttime;
        }
        if (maxEndtime < jobNode.endtime || maxEndtime === -1) {
            maxEndtime = jobNode.endtime;
        }
        times.push([jobNode.starttime, jobNode.endtime]);
    }
    var length = maxEndtime - minStarttime;

    times.sort(function (t1, t2) {
        if (t1[0] !== t2[0]) {
            return t1[0] > t2[0] ? 1 : -1;
        }
        return t1[1] > t2[1] ? 1 : -1;
    });

    let merged_times = [];
    merged_times.push(times[0]);
    for (const [start, end] of times) {
        let [start1, end1] = merged_times[merged_times.length - 1];
        if (start > start1) {
            merged_times.push([start, end]);
        } else {
            if (end1 < end) {
                merged_times[merged_times.length - 1][1] = end;
            }
        }
    }

    for (let i = 0; i + 1 < merged_times.length; i++) {
        let end1 = merged_times[i][1];
        let start2 = merged_times[i + 1][0];
        length -= (start2 - end1);
    }
    timeline.range.options.min = minStarttime - length * 0.1;
    timeline.range.options.max = maxEndtime + length * 0.1;

    timeline.range.options.hiddenDates = []; // update hidden dates
    for (let i = 0; i + 1 < merged_times.length; i++) {
        let end1 = merged_times[i][1];
        let start2 = merged_times[i + 1][0];
        if (start2 - end1 > length * 0.1) {
            timeline.range.options.hiddenDates.push({start: end1 + length * 0.05, end: start2 - length * 0.05});
        }
    }

    jobs.forEach(function (job) { // get rid of deleted jobs
        if (!survivingJobs.has(job.id)) {
            jobs.remove(job.id);
        }
    })
}

function updateAllArrows(jobMap) {
    allArrows = [];
    let id = 0;
    for (const [jobID, jobNode] of jobMap.entries()) {
        for (const dependency of jobNode.dependencies) {
            allArrows.push({
                id: id,
                id_item_1: jobID,
                id_item_2: dependency
            });
            id++;
        }
    }
}


//// Setting up the Timeline objects ////

const jobs = new vis.DataSet();

// Order by job length
function customOrder(jobA, jobB) {
    return (jobB.end - jobB.start) - (jobA.end - jobA.start);
}

// Configuration for the Timeline
const options = {
    order: customOrder,
    stack: true,
    verticalScroll: true,
    zoomKey: "ctrlKey",
    width: '100%',
    maxHeight: '700px',
    tooltip: {
        followMouse: true,
        overflowMethod: "cap",
    },
    multiselect: true,
    margin: {
        item: {horizontal: -1, vertical: 0}
    }
};
const groups = new vis.DataSet([
    {id: 0, content: "", value: 0}
]);

// Create the Timeline
const timeline = new vis.Timeline(container, jobs, options, groups);


//// Graph construction  ////

class JobNode {
    constructor(job) {
        this.job = job;
        this.starttime = parseInt(job.starttime.toString().slice(0, -6));
        this.endtime = parseInt(job.endtime.toString().slice(0, -6));

        this.dependencies = new Set();
        this.hypotheticalEndtime = -1;
    }
}

function fillAllDependencies(dependencies, jobMap) {
    for (let dep of dependencies) {
        let writer = dep.writer;
        let reader = dep.reader;

        if (jobMap.has(writer) && jobMap.has(reader)) {
            jobMap.get(reader).dependencies.add(writer);
        }
    }
}

function assignParents(jobMap, topSortedJobs) {
    for (const jobID of topSortedJobs) {
        let jobNode = jobMap.get(jobID);
        for (const dependencyID of jobNode.dependencies) {
            let dependency = jobMap.get(dependencyID);
            if (dependency.hypotheticalEndtime >= jobNode.hypotheticalStarttime) {
                jobNode.hypotheticalStarttime = dependency.hypotheticalEndtime;
            }
        }
        jobNode.hypotheticalEndtime = jobNode.hypotheticalStarttime + (jobNode.endtime - jobNode.starttime);
    }
}


//// Graph processing ////

function dfsTopSort(jobID, jobMap, topSortedJobs, visited) {
    let job = jobMap.get(jobID);
    visited.add(jobID);
    for (let dependency of job.dependencies) {
        if (!jobMap.has(dependency)) {
            job.dependencies.delete(dependency);
            continue;
        }
        if (!visited.has(dependency)) {
            dfsTopSort(dependency, jobMap, topSortedJobs, visited);
        }
    }
    topSortedJobs.push(jobID);
}

function topSort(jobMap) {
    let topSortedJobs = [];
    let visited = new Set();
    for (const [jobID, _] of jobMap.entries()) {
        if (!visited.has(jobID)) {
            dfsTopSort(jobID, jobMap, topSortedJobs, visited);
        }
    }
    return topSortedJobs;
}

function dfsTransitiveReduction(jobID, jobMap, ancestors, visited) {
    let job = jobMap.get(jobID);
    if (!visited.has(jobID)) {
        ancestors.add(jobID);
        visited.add(jobID);
    }

    for (let childID of job.dependencies) {
        let child = jobMap.get(childID);
        for (let grandchildID of child.dependencies) {
            for (let ancestorID of ancestors) {
                let ancestor = jobMap.get(ancestorID);
                ancestor.dependencies.delete(grandchildID);
            }
        }
        dfsTransitiveReduction(childID, jobMap, ancestors, visited);
    }
    ancestors.delete(jobID);
}

function transitiveReduction(jobMap) {
    let ancestors = new Set();
    let visited = new Set();
    for (const [jobID, _] of jobMap.entries()) {
        if (!visited.has(jobID)) {
            dfsTransitiveReduction(jobID, jobMap, ancestors, visited);
        }
    }
}


//// Setting of initial state ////

let selectedJobsIds = [];
const jobMap = new Map();

let allArrows = [];
const visibleArrows = [];
const arrowObject = new Arrow(timeline, visibleArrows);


//// Processing user-initiated events ////

function onSelect(items) {
    clearVisibleArrows();
    const added = [];
    selectedJobsIds = [];
    for (const item of items) {
        addNodeArrows(item, added);
        added.push(item);
        selectedJobsIds.push(item);
    }
}

timeline.on('select', properties => onSelect(properties.items));

timeline.on('click', function (properties) {
    if (properties.item == null) {
        document.getElementById("job").innerHTML = "";
        document.getElementById("stale").innerHTML = "";
        document.getElementById("label").innerHTML = "";
        document.getElementById("directory").innerHTML = "";
        document.getElementById("commandline").innerHTML = "";
        document.getElementById("environment").innerHTML = "";
        document.getElementById("stack").innerHTML = "";
        document.getElementById("stdin_file").innerHTML = "";
        document.getElementById("starttime").innerHTML = "";
        document.getElementById("endtime").innerHTML = "";
        document.getElementById("wake_start").innerHTML = "";
        document.getElementById("wake_cmdline").innerHTML = "";
        document.getElementById("stdout_payload").innerHTML = "";
        document.getElementById("stderr_payload").innerHTML = "";
        document.getElementById("usage").innerHTML = "";
        document.getElementById("visible").innerHTML = "";
        document.getElementById("outputs").innerHTML = "";
        document.getElementById("tags").innerHTML = "";
        return;
    }
    let job = jobMap.get(parseInt(properties.item)).job;
    document.getElementById("job").innerHTML = job.job
    document.getElementById("stale").innerHTML = job.stale ? "true" : "false";
    document.getElementById("label").innerHTML = job.label;
    document.getElementById("directory").innerHTML = job.directory;
    document.getElementById("commandline").innerHTML = job.commandline;
    document.getElementById("environment").innerHTML = job.environment;
    document.getElementById("stack").innerHTML = job.stack;
    document.getElementById("stdin_file").innerHTML = job.stdin_file;

    const starttime = new Date(job.starttime / 1e6);
    document.getElementById("starttime").innerHTML = starttime.toISOString();
    const endtime = new Date(job.endtime / 1e6);
    document.getElementById("endtime").innerHTML = endtime.toISOString();
    const wake_start = new Date(job.wake_start / 1e6);
    document.getElementById("wake_start").innerHTML = wake_start.toISOString();

    document.getElementById("wake_cmdline").innerHTML = job.wake_cmdline;
    document.getElementById("stdout_payload").innerHTML = job.stdout_payload;
    document.getElementById("stderr_payload").innerHTML = job.stderr_payload;

    document.getElementById("usage").innerHTML = job.usage;
    document.getElementById("visible").innerHTML = job.visible;
    document.getElementById("outputs").innerHTML = job.outputs;
    document.getElementById("tags").innerHTML = job.tags;
});

window.addEventListener("message", event => {
    const message = event.data;
    const newJobReflections = message.jobReflections;
    const newFileDependencies = message.fileDependencies;
    processChanges(newJobReflections, newFileDependencies);
});

function processChanges(newJobReflections, newFileDependencies) {
    jobMap.clear();

    for (const job of newJobReflections) {
        jobMap.set(job.job, new JobNode(job));
    }

    fillAllDependencies(newFileDependencies, jobMap);

    let topSortedJobs = topSort(jobMap);
    assignParents(jobMap, topSortedJobs);
    transitiveReduction(jobMap);

    updateData(jobMap);

}

function updateData(jobMap) {
    clearVisibleArrows();

    let selectedJobsIdsSet = new Set(selectedJobsIds);
    let selectedJobs = jobs.get().filter(job => selectedJobsIdsSet.has(job.id));

    updateJobs(jobMap);
    const newJobsMap = new Map();
    for (const job of jobs.get()) {
        newJobsMap.set(job.id, job);
    }
    let filteredSelectedJobsIds = selectedJobs.filter(function (selectedJob) {
        const newJob = newJobsMap.get(selectedJob.id);
        if (newJob === null || newJob === undefined) {
            return 0;
        }
        return jobsEqual(selectedJob, newJob);
    }).map(job => job.id);
    timeline.setSelection(filteredSelectedJobsIds);
    onSelect(filteredSelectedJobsIds);

    updateAllArrows(jobMap);
}

function jobsEqual(jobA, jobB) {
    return (jobA.id === jobB.id &&
        jobA.starttime === jobB.starttime &&
        jobA.endtime === jobB.endtime);
}


// Process initial data
processChanges(JSON.parse(document.getElementById("jobReflections").textContent),
    JSON.parse(document.getElementById("fileDependencies").textContent));
