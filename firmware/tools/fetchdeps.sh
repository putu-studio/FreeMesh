#!/bin/bash
# Usage: fetchdeps.sh <repo dir>
# Fetches each zip dependency with git, then points the .ini files at the local copy (symlink://).
R=$1; D=$(cd "$(dirname "$0")" && pwd)/deps
for u in $(grep -rhoE "https://github.com/[^ /]+/[^ /]+/archive/[^ ]+\.zip" "$R" --include=*.ini | sort -u); do
  repo=$(echo "$u" | sed -E 's#https://github.com/([^/]+/[^/]+)/archive/.*#\1#')
  ref=$(echo "$u" | sed -E 's#.*/archive/(.+)\.zip#\1#')
  dir=$D/$(echo "$repo" | tr / _)-$(echo "$ref" | tr / _)
  if [ ! -d "$dir/.git" ]; then
    git init -q "$dir" && git -C "$dir" fetch -q --depth 1 "https://github.com/$repo" "$ref" && git -C "$dir" checkout -q FETCH_HEAD || { echo "FAILED $repo $ref"; continue; }
  fi
  grep -rl "$u" "$R" --include=*.ini | xargs -r sed -i "s#$u#symlink://$dir#g"
  echo "ok $repo $ref"
done
