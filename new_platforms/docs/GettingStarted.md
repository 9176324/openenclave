Getting Started with Open Enclave: TrustZone and SGX
=========================================

This getting started guide includes preview support for new Trusted Execution Environment (TEE) platforms.

* ARM TrustZone with a Linux Host
* Intel SGX with a Windows Host (Intel SDK)
  
Support for a Windows Host on ARM and native Open Enclave support for Windows SGX will be added in the future.

In addition, this preview includes support for testing your enclave under simulation when developing on Windows. 
Support for simulation when developing on Linux is coming soon.

For Linux Host on SGX, see the Linux SGX Open Enclave [Getting Started](../../docs/GettingStartedDocs/GettingStarted.md) guide.

## Multi-Platform Support

The main goal of the Open Enclave SDK is to provide a single API surface that works across multiple platforms.
With the addition of ARM TrustZone Trusted Applications (TA's) as a supported platform
you can write a single Host and Enclave application,
then build and run the Enclave as an SGX Enclave or a TrustZone TA. 
For under-the-cover details on the differences between SGX and ARM TrustZone, see [Understanding SGX and TrustZone](sgx_trustzone_arch.md)

## Development Environment

This SDK supports developing your Host and Enclave on either a Windows or a Linux development environment. 
The selection of your development environment should be guided by the Rich Execution Environment (REE) the host application will be running on. 

If you want to dive right in, use the table below.

| REE     | TEE                                               |
| ------- | :------------------------------------------------ |
| Linux   | [ARM TrustZone](linux_arm_dev.md)                    |
| Windows | [Intel SGX](win_sgx_dev.md)                          |
| Windows | [Simulated SGX](win_sgx_dev.md#sgx-simulation)                  |
| Windows | [Simulated TrustZone (OP-TEE)](win_sgx_dev.md#op-tee-simulation) |

## Linux Host and ARM TrustZone TA

ARM TrustZone TA support currently targets the Scalys Grapeboard, based on the NXP Layerscape LS1012A, as a reference platform. 
This reference platform runs OP-TEE to support ARM TrustZone TAs. For more details, see the links below: 

* [Getting Started with the Grapeboard](grapeboard.md)
* [Building an Open Enclave TrustZone TA on Linux](linux_arm_dev.md)
* [Running the Sample Client on the Grapeboard](sample_sockets.md#building-for-grapeboard)

## Windows Host and SGX Enclave (Intel SDK Required)

Windows SGX Enclave support works on any device with a SGX-capable processor.
To find out if your processor supports SGX, consult [Intel ARK](ark.inte.com). An example device with such support is the NUC7i3BNK.

* [Building an Open Enclave SGX Enclave on Windows](win_sgx_dev.md)
* [Running the Sample Server on SGX](sample_sockets.md#building-for-sgx)

## Windows Host with SGX and OP-TEE Simulation

Simulation provides a development environment to quickly write, test, and debug your REE and TEE code. 
This SDK provides simulation support for SGX using Intel Simulation SDK and for TrustZone using OP-TEE simulation.
Both environments can be debugged using Visual Studio.

* [Building an Open Enclave Simulation on Windows](win_sgx_dev.md#simulation)
* [Simulating the Sample Server and Client](sample_sockets.md#building-for-simulation)

## Multi-Platform Sample

Running through the steps above will produce enclaves for both SGX and ARM using the same Open Enclave sample code.
To demonstrate how these samples can come together and provide TEE to TEE communication, see the [IoTEdge Socket Sample](sample_edge_sockets.md).

## Beyond the Samples
* [Open Enclave API Documentation](openenclaveapi.md)
* [Developing your own enclave](new_platform_dev.md)
* [Understanding SGX and TrustZone](sgx_trustzone_arch.md)

