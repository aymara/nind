#!/bin/bash

dockerImage=$1
packageDir=$2
id=$(docker run -d "$dockerImage" sleep infinity)
echo -n "export fullname=$packageDir/" > ./envFile
docker exec $id ls $packageDir >> ./envFile
echo "export filename=\$(basename -- \$fullname)" >> ./envFile
echo "export trimmed=\$(basename \$filename .deb)" >> ./envFile
echo "tmp=\${trimmed#*-}" >> ./envFile
echo "export version=\${tmp%-*}" >> ./envFile
echo "filename=$(. envFile && echo $filename)" >> $GITHUB_OUTPUT
echo "version=$(. envFile && echo $version)"  >> $GITHUB_OUTPUT
. envFile && docker cp $id:$fullname - > ./$filename
docker stop $id
docker rm -v $id
