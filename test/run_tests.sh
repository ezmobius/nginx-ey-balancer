#!/bin/sh
for i in test_*; do
  echo -n "$i: ";
  ruby $i && echo "PASS" || echo "FAIL"; 
done 
