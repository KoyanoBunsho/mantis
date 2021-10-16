# build
docker-compose exec build_mantis mantis build -s 20 -i raw/incqfs.lst -o raw/
# MST
docker-compose exec build_mantis mantis mst -p raw/ -t 8 -k
# Query
docker-compose exec build_mantis mantis query -p raw/ -o query.res raw/input_txns.fa

