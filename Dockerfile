# Use a standard GCC image as the base
FROM gcc:latest

# Set the working directory inside the container
WORKDIR /app

# Copy the source code into the container
COPY pm_sim.c .

# Compile the simulator
RUN gcc -Wall -o pm_sim pm_sim.c -lpthread

# Set the entrypoint to the compiled binary
# This allows passing arguments (thread files) easily
ENTRYPOINT ["./pm_sim"]
