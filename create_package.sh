#!/bin/bash
# generic create_package.sh
PACKAGE_NAME="samsung-sdi-victron-integration"
VERSION="v1.1.0"
OUTPUT_FILE="venus-data.tar.gz"

echo "Creating Victron Blind Install Package ($OUTPUT_FILE)..."

# 1. Create Staging Directory
rm -rf temp_build
mkdir -p temp_build/$PACKAGE_NAME

# 2. Copy Project Files
# We explicitly exclude git, temp files, and the output archive itself to keep it clean
cp -r ./* temp_build/$PACKAGE_NAME/
rm -rf temp_build/$PACKAGE_NAME/.git
rm -rf temp_build/$PACKAGE_NAME/temp_build
rm -f temp_build/$PACKAGE_NAME/$OUTPUT_FILE
rm -f temp_build/$PACKAGE_NAME/create_package.sh

# 3. Create 'rc' folder for Blind Install Trigger
# This is the magic folder that Venus OS looks for to auto-execute logic
mkdir -p temp_build/rc

cat > temp_build/rc/startup <<EOF
#!/bin/sh
echo "Samsung SDI Blind Install Started..." >> /var/log/samsung_install.log
cd /data/$PACKAGE_NAME
chmod +x install.sh
./install.sh >> /var/log/samsung_install.log 2>&1
EOF
chmod +x temp_build/rc/startup

# 4. Create the Archive
# Must change dir so that 'rc' and project folder are at the root of the archive
cd temp_build
tar -czf ../$OUTPUT_FILE .

# 5. Cleanup
cd ..
rm -rf temp_build

echo "Done!"
echo "---------------------------------------------------------"
echo "File created: $OUTPUT_FILE"
echo "USAGE:"
echo "1. Copy $OUTPUT_FILE to the root of a USB stick or SD card."
echo "2. Insert into the Victron Cerbo GX."
echo "3. Reboot the device."
echo "4. The contents will be auto-extracted to /data/"
echo "   and /data/rc/startup will run the installer."
echo "---------------------------------------------------------"
