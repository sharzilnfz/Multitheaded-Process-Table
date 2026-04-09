# Use a standard GCC image as the base
FROM gcc:latest

# Set the working directory inside the container
WORKDIR /app

# Copy all files into the container
# This includes pm_sim.c and any .txt script files
COPY . .

# Compile the simulator
RUN gcc -Wall -o /usr/local/bin/pm_sim pm_sim.c -lpthread

# Set the entrypoint to the compiled binary
ENTRYPOINT ["pm_sim"]
