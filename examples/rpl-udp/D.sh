java -mx512m -jar ../../../tools/cooja/dist/cooja.jar -nogui=../../20.csc -random-seed=12345
echo "copying first file"
cp COOJA.testlog D-$1-1

java -mx512m -jar ../../../tools/cooja/dist/cooja.jar -nogui=../../20.csc -random-seed=12346
echo "copying second file"
cp COOJA.testlog D-$1-2

java -mx512m -jar ../../../tools/cooja/dist/cooja.jar -nogui=../../20.csc -random-seed=12347
echo "copying third file"
cp COOJA.testlog D-$1-3

java -mx512m -jar ../../../tools/cooja/dist/cooja.jar -nogui=../../20.csc -random-seed=12348
echo "copying fourth file"
cp COOJA.testlog D-$1-4

java -mx512m -jar ../../../tools/cooja/dist/cooja.jar -nogui=../../20.csc -random-seed=12349
echo "copying fifth file"
cp COOJA.testlog D-$1-5
