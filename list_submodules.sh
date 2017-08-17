#!/bin/bash

git submodule--helper list | grep "libs/" | awk '{print $4" "$2}'
