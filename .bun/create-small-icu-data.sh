#!/bin/bash
set -e  # Exit on error

# Create a clean working directory
rm -rf icu_trim
mkdir -p icu_trim
cd icu_trim

# Copy the original data file here
cp ../$1 .

# First, list all available items
icupkg -l $1 > all_items.txt

# Create a file with ONLY the items we want to keep
cat > keep_list.txt << EOF
coll/root.res
coll/en.res
curr/root.res
curr/en.res
zone/root.res
zone/en.res
stringprep/root.res
stringprep/en.res
translit/root.res
translit/en.res
unit/root.res
unit/en.res
pool.res
supplementalData.res
zoneinfo64.res
likelySubtags.res
EOF

# Create a file with items to remove
cat > remove_list.txt << EOF
cnvalias.icu
postalCodeData.res
genderList.res
brkitr/root.res
unames.icu
EOF

# Create extraction directory
mkdir -p extracted

# Extract one item at a time to ensure precision
while read item; do
  # Check if item exists
  if grep -q "^$item$" all_items.txt; then
    echo "Extracting $item"
    icupkg -x "$item" -d extracted $1
  else
    echo "Item not found: $item"
  fi
done < keep_list.txt

# Create a new empty package
icupkg -c -C "Trim down ICU to just a certain locale set, needed for node.js use." new trimmed.dat

# Add extracted files, skipping ones in remove_list
while read file; do
  rel_path=${file#extracted/}
  
  # Check if this file should be removed
  should_remove=0
  while read remove_item; do
    if [ "$rel_path" = "$remove_item" ]; then
      should_remove=1
      break
    fi
  done < remove_list.txt
  
  if [ $should_remove -eq 0 ]; then
    echo "Adding $rel_path"
    icupkg -a "$file" --ignore-deps trimmed.dat
  else
    echo "Skipping $rel_path (in remove list)"
  fi
done < <(find extracted -type f)

# Check the final size
cp trimmed.dat ../$2
echo "Filtered ICU package created according to JSON configuration"
