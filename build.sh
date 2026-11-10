#!/bin/sh
cd "$(dirname "$0")"
exec xcodebuild -project gs2print.xcodeproj -scheme gs2print -configuration Debug -derivedDataPath build
